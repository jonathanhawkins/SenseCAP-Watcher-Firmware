#include "esp_log.h"
#include "driver/i2c.h"        // Legacy I2C Driver
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_bit_defs.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "esp_io_expander.h"
#include "esp_io_expander_pca95xx_16bit.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_spd2010.h"
#include "esp_lcd_touch_spd2010.h"
#include "esp_lcd_panel_io.h"
#include "board.h"
#include <stdbool.h>
#include <assert.h>
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "esp_system.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "ui.h"

static const char *TAG = "board";

// --- Defines directly from sensecap-watcher.h ---
/* Audio I2S Pins */
#define BSP_AUDIO_I2S_NUM  (0)
#define BSP_AUDIO_I2S_MCLK (GPIO_NUM_10)
#define BSP_AUDIO_I2S_SCLK (GPIO_NUM_11)
#define BSP_AUDIO_I2S_LRCK (GPIO_NUM_12)
#define BSP_AUDIO_I2S_DSIN (GPIO_NUM_15)
#define BSP_AUDIO_I2S_DOUT (GPIO_NUM_16)

/* I2C Pins */
#define BSP_GENERAL_I2C_NUM (0)
#define BSP_GENERAL_I2C_SDA (GPIO_NUM_47)
#define BSP_GENERAL_I2C_SCL (GPIO_NUM_48)
#define BSP_GENERAL_I2C_CLK (400000)

/* QSPI / LCD Pins to Silence */
#define BSP_LCD_SPI_NUM     (SPI3_HOST)
#define BSP_LCD_SPI_CS      (GPIO_NUM_45)
#define BSP_LCD_GPIO_RST    (GPIO_NUM_NC)
#define BSP_LCD_GPIO_DC     (GPIO_NUM_1)
#define BSP_LCD_GPIO_BL     (GPIO_NUM_8)
#define BSP_SPI3_HOST_PCLK  (GPIO_NUM_7)
#define BSP_SPI3_HOST_DATA0 (GPIO_NUM_9)
#define BSP_SPI3_HOST_DATA1 (GPIO_NUM_1)
#define BSP_SPI3_HOST_DATA2 (GPIO_NUM_14)
#define BSP_SPI3_HOST_DATA3 (GPIO_NUM_13)

#define BSP_TOUCH_I2C_NUM (1)
#define BSP_TOUCH_I2C_SDA (GPIO_NUM_39)
#define BSP_TOUCH_I2C_SCL (GPIO_NUM_38)
#define BSP_TOUCH_I2C_CLK (400000)

/* IO Expander */
#define BSP_IO_EXPANDER_INT    (GPIO_NUM_2)
#define DRV_IO_EXP_INPUT_MASK  (0x20ff) // P0.0 ~ P0.7 | P1.3
#define DRV_IO_EXP_OUTPUT_MASK (0xDf00) // P1.0 ~ P1.7 & ~P1.3

#define BSP_KNOB_BTN (IO_EXPANDER_PIN_NUM_3)

#define BSP_SSCMA_CLIENT_RST (IO_EXPANDER_PIN_NUM_7)

/* Power Rails (IO Expander Pins) */
#define BSP_PWR_SDCARD           (IO_EXPANDER_PIN_NUM_8)
#define BSP_PWR_LCD              (IO_EXPANDER_PIN_NUM_9)
#define BSP_PWR_SYSTEM           (IO_EXPANDER_PIN_NUM_10)
#define BSP_PWR_AI_CHIP          (IO_EXPANDER_PIN_NUM_11)
#define BSP_PWR_CODEC_PA         (IO_EXPANDER_PIN_NUM_12)
#define BSP_PWR_BAT_DET          (IO_EXPANDER_PIN_NUM_13)
#define BSP_PWR_GROVE            (IO_EXPANDER_PIN_NUM_14)
#define BSP_PWR_BAT_ADC          (IO_EXPANDER_PIN_NUM_15)
#define BSP_PWR_START_UP         (BSP_PWR_SDCARD | BSP_PWR_LCD | BSP_PWR_SYSTEM | BSP_PWR_AI_CHIP | BSP_PWR_CODEC_PA | BSP_PWR_GROVE | BSP_PWR_BAT_ADC | BSP_SSCMA_CLIENT_RST)

/* Charge-detect inputs (PCA9535 P0.x; part of DRV_IO_EXP_INPUT_MASK 0x20ff).
   Battery-present is BSP_PWR_BAT_DET (pin 13) above — also an input bit.
   CHRG_DET alone can't tell "on power" from "on battery" (it reads HIGH both
   when the pack is full-on-charger and when unplugged), so VBUS_IN_DET (USB
   present, active-low) drives the charging indicator. */
#define BSP_PWR_CHRG_DET         (IO_EXPANDER_PIN_NUM_0)
#define BSP_PWR_VBUS_IN_DET      (IO_EXPANDER_PIN_NUM_2)
/* Battery voltage sense: ADC1 ch2 (GPIO3), 2.5 dB atten, 4.1x divider
   (62k+20k / 20k). Divider rail is powered by BSP_PWR_BAT_ADC at boot.
   Mirrors components/sensecap-watcher/include/sensecap-watcher.h. */
#define BSP_BAT_ADC_CHAN         (ADC_CHANNEL_2)
#define BSP_BAT_ADC_ATTEN        (ADC_ATTEN_DB_2_5)
#define BSP_SSCMA_CLIENT_RST_LOW (IO_EXPANDER_PIN_NUM_7)
#define BSP_TOUCH_I2C_CLK        (400000)

/* Codec Addresses */
#define DRV_ES8311_I2C_ADDR  (0x18)
#define DRV_ES7243_I2C_ADDR  (0x13)
#define DRV_ES7243E_I2C_ADDR (0x14)
#define DRV_RTC_I2C_ADDR     (0x51)

/* Audio Settings */
#define DRV_AUDIO_SAMPLE_RATE (48000)
#define DRV_AUDIO_SAMPLE_BITS (16)
#define DRV_AUDIO_CHANNELS    (1)
#define DRV_AUDIO_MIC_GAIN    (36.0)  // was 27 (ES8311 quantized to 24dB → far-field voice too quiet); see media.c MIC_ACTIVE_GAIN_DB
#define DRV_AUDIO_I2S_CHANNEL (1)

/* LCD Settings (match SenseCAP firmware) */
#define DRV_LCD_H_RES             (412)
#define DRV_LCD_V_RES             (412)
#define DRV_LCD_PIXEL_CLK_HZ      (10 * 1000 * 1000)  // Reduced from 20MHz to avoid SPI queue errors
#define DRV_LCD_CMD_BITS          (32)
#define DRV_LCD_PARAM_BITS        (8)
#define DRV_LCD_RGB_ELEMENT_ORDER (LCD_RGB_ELEMENT_ORDER_RGB)
#define DRV_LCD_BITS_PER_PIXEL    (16)
#define DRV_LCD_SWAP_XY           (0)
#define DRV_LCD_MIRROR_X           (0)
#define DRV_LCD_MIRROR_Y           (0)

#define DRV_LCD_BL_ON_LEVEL   (1)
#define DRV_LCD_LEDC_DUTY_RES (LEDC_TIMER_10_BIT)
#define DRV_LCD_LEDC_CH       (1)

#ifndef CONFIG_LVGL_DRAW_BUFF_HEIGHT
#define CONFIG_LVGL_DRAW_BUFF_HEIGHT (40)  // Use partial buffer (40 lines) - full screen (412) causes DMA memory errors
#endif
#define LVGL_DRAW_BUFF_HEIGHT (CONFIG_LVGL_DRAW_BUFF_HEIGHT)
#define LVGL_DRAW_BUFF_DOUBLE (1)  // Match factory - enable double buffering for screen transitions

// Helper macros
#define BSP_I2S_GPIO_CFG                                                                                                                                                                               \
    { \
        .mclk = BSP_AUDIO_I2S_MCLK, .bclk = BSP_AUDIO_I2S_SCLK, .ws = BSP_AUDIO_I2S_LRCK, .dout = BSP_AUDIO_I2S_DOUT, .din = BSP_AUDIO_I2S_DSIN, \
        .invert_flags = { \
            .mclk_inv = false, \
            .bclk_inv = false, \
            .ws_inv = false, \
        }, \
    }

#define BSP_I2S_SLOT_CONFIG(bits_per_sample, mono_or_stereo)                                                                                                                                           \
    { .data_bit_width = bits_per_sample,                                                                                                                                                               \
        .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,                                                                                                                                                     \
        .slot_mode = mono_or_stereo,                                                                                                                                                                   \
        .slot_mask = I2S_STD_SLOT_BOTH,                                                                                                                                                                \
        .ws_width = bits_per_sample,                                                                                                                                                                   \
        .ws_pol = false,                                                                                                                                                                               \
        .bit_shift = true,                                                                                                                                                                             \
        .left_align = true,                                                                                                                                                                            \
        .big_endian = false,                                                                                                                                                                           \
        .bit_order_lsb = false }

#define BSP_I2S_DUPLEX_MONO_CFG(_sample_rate)                                                                                                                                                          \
    {                                                                                                                                                                                                  \
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(_sample_rate),                                                                                                                                           \
        .slot_cfg = BSP_I2S_SLOT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),                                                                                                                 \
        .gpio_cfg = BSP_I2S_GPIO_CFG,                                                                                                                                                                  \
    }

// Global variables
static i2c_port_t i2c0_port = -1;
// static i2c_master_bus_handle_t i2c1_bus_handle = NULL; // Unused for now
static esp_io_expander_handle_t io_exp_handle = NULL;
static esp_codec_dev_handle_t play_dev_handle;
static esp_codec_dev_handle_t record_dev_handle;
static SemaphoreHandle_t codec_mutex = NULL;
static i2s_chan_handle_t i2s_tx_chan = NULL;
static i2s_chan_handle_t i2s_rx_chan = NULL;
static const audio_codec_data_if_t *i2s_data_if = NULL;

static esp_err_t bsp_i2c0_bus_init(void)
{
    if (i2c0_port != -1)
    {
        return ESP_OK;
    }

    // --- Legacy Init Recreation: Silence other buses ---
    // Match working main BSP: drive ALL relevant hardware pins to 0 early
    const gpio_config_t log_pins_cfg = {
        .pin_bit_mask = (1ULL << BSP_SPI3_HOST_PCLK),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLUP_DISABLE,
    };
    gpio_config(&log_pins_cfg);
    ESP_LOGI(TAG, "Current level of GPIO 7 (PCLK) before silence: %d", gpio_get_level(BSP_SPI3_HOST_PCLK));
    ESP_LOGI(TAG, "GPIO Audit - SDA(39): %d, SCL(38): %d", gpio_get_level(BSP_TOUCH_I2C_SDA), gpio_get_level(BSP_TOUCH_I2C_SCL));
    gpio_set_level(BSP_TOUCH_I2C_SDA, 0);
    gpio_set_level(BSP_TOUCH_I2C_SCL, 0);
    ESP_LOGI(TAG, "GPIO Audit AFTER Silence - SDA(39): %d, SCL(38): %d", gpio_get_level(BSP_TOUCH_I2C_SDA), gpio_get_level(BSP_TOUCH_I2C_SCL));

    // v23: Restore I2C1 pins (38/39) to early zeroing (Strict parity with core BSP)
    const gpio_config_t io_config = {
        .pin_bit_mask = (1ULL << BSP_TOUCH_I2C_SDA) | (1ULL << BSP_TOUCH_I2C_SCL) | (1ULL << BSP_SPI3_HOST_PCLK) | (1ULL << BSP_SPI3_HOST_DATA0) | (1ULL << BSP_SPI3_HOST_DATA1)
                        | (1ULL << BSP_SPI3_HOST_DATA2) | (1ULL << BSP_SPI3_HOST_DATA3) | (1ULL << BSP_LCD_SPI_CS) | (1ULL << BSP_LCD_GPIO_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_config);
    gpio_set_level(BSP_TOUCH_I2C_SDA, 0);
    gpio_set_level(BSP_TOUCH_I2C_SCL, 0);
    ESP_LOGI(TAG, "GPIO Audit AFTER Output Drive - SDA(39): %d, SCL(38): %d", gpio_get_level(BSP_TOUCH_I2C_SDA), gpio_get_level(BSP_TOUCH_I2C_SCL));
    gpio_set_level(BSP_LCD_SPI_CS, 0);
    gpio_set_level(BSP_LCD_GPIO_BL, 0);
    gpio_set_level(BSP_SPI3_HOST_PCLK, 0);
    gpio_set_level(BSP_SPI3_HOST_DATA0, 0);
    gpio_set_level(BSP_SPI3_HOST_DATA1, 0);
    gpio_set_level(BSP_SPI3_HOST_DATA2, 0);
    gpio_set_level(BSP_SPI3_HOST_DATA3, 0);
    // ----------------------------------------------------

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BSP_GENERAL_I2C_SDA,
        .scl_io_num = BSP_GENERAL_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_DISABLE, // Disabled as per legacy
        .scl_pullup_en = GPIO_PULLUP_DISABLE, // Disabled as per legacy
        .master.clk_speed = BSP_GENERAL_I2C_CLK,
    };

    esp_err_t ret = i2c_param_config(BSP_GENERAL_I2C_NUM, &conf);
    if (ret != ESP_OK)
        return ret;

    ret = i2c_driver_install(BSP_GENERAL_I2C_NUM, conf.mode, 0, 0, ESP_INTR_FLAG_SHARED);
    if (ret != ESP_OK)
        return ret;

    i2c0_port = BSP_GENERAL_I2C_NUM;
    return ESP_OK;
}

static i2c_port_t i2c1_port = -1;
static esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_panel_io_handle_t panel_io_handle = NULL;
static lv_disp_t *lvgl_disp = NULL;
static lv_indev_t *lvgl_touch_indev = NULL;
static esp_lcd_touch_handle_t touch_handle = NULL;

static esp_err_t bsp_i2c1_bus_init(void)
{
    if (i2c1_port != -1)
    {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing I2C1 (Touch Bus)...");
    ESP_LOGI(TAG, "I2C1 pre-init levels: SDA=%d SCL=%d", gpio_get_level(BSP_TOUCH_I2C_SDA), gpio_get_level(BSP_TOUCH_I2C_SCL));

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BSP_TOUCH_I2C_SDA,
        .scl_io_num = BSP_TOUCH_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = BSP_TOUCH_I2C_CLK,
    };

    esp_err_t ret = i2c_param_config(BSP_TOUCH_I2C_NUM, &conf);
    if (ret != ESP_OK)
        return ret;

    ret = i2c_driver_install(BSP_TOUCH_I2C_NUM, conf.mode, 0, 0, ESP_INTR_FLAG_SHARED);
    if (ret != ESP_OK)
        return ret;

    ESP_LOGI(TAG, "I2C1 initialized");
    i2c1_port = BSP_TOUCH_I2C_NUM;
    return ESP_OK;
}

static esp_err_t bsp_lcd_brightness_set(int brightness_percent);

static esp_err_t bsp_audio_init(void)
{
    if (i2s_tx_chan && i2s_rx_chan)
    {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BSP_AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.intr_priority = 4;
    // Deepen the TX DMA ring from the IDF default (6 desc x 240 frames = ~30 ms @ 48 kHz)
    // to ~60 ms. WRITE_DIAG instrumentation on i2s_render_write measured the av_render
    // consumer thread being preempted (WiFi-RX bursts / LiveKit peer_task) for gaps up to
    // 60 ms during a greeting (steady-state worst 35 ms); a 30 ms ring underran on those
    // gaps -> the "crackle while it talks". Raising the av_render thread priority is a
    // KNOWN crash (priority inversion vs peer_task — see media.c), so the ring depth is the
    // lever. NOTE: this is a memory-constrained device — the I2S DMA ring competes with the
    // 32 KB LCD strip for INTERNAL DMA RAM. dma_desc_num=20 (~100 ms) caused LCD ESP_ERR_NO_MEM
    // and broke the LiveKit connect; 12 (~60 ms) covers the 35 ms steady-state gap with margin
    // at +5.8 KB. dma_frame_num stays 240 so RX/capture granularity (and the duplex-reconnect
    // fix) are unchanged. IDF only requires dma_desc_num >= 2 (i2s_common.c:948).
    chan_cfg.dma_desc_num = 12;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx_chan, &i2s_rx_chan));

    i2s_std_config_t std_cfg_default = BSP_I2S_DUPLEX_MONO_CFG(DRV_AUDIO_SAMPLE_RATE);
    i2s_std_config_t *p_i2s_cfg = &std_cfg_default;

    if (i2s_tx_chan != NULL)
    {
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx_chan, p_i2s_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx_chan));
    }
    if (i2s_rx_chan != NULL)
    {
        p_i2s_cfg->slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx_chan, p_i2s_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(i2s_rx_chan));
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = BSP_AUDIO_I2S_NUM,
        .rx_handle = i2s_rx_chan,
        .tx_handle = i2s_tx_chan,
    };
    i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    assert(i2s_data_if);

    return ESP_OK;
}

static esp_err_t bsp_spi_bus_init(void)
{
    static bool initialized = false;
    if (initialized)
    {
        return ESP_OK;
    }

    const spi_bus_config_t qspi_cfg = {
        .sclk_io_num = BSP_SPI3_HOST_PCLK,
        .data0_io_num = BSP_SPI3_HOST_DATA0,
        .data1_io_num = BSP_SPI3_HOST_DATA1,
        .data2_io_num = BSP_SPI3_HOST_DATA2,
        .data3_io_num = BSP_SPI3_HOST_DATA3,
        .max_transfer_sz = DRV_LCD_H_RES * DRV_LCD_V_RES * DRV_LCD_BITS_PER_PIXEL / 8 / 16,  // Divide by 16 to limit DMA buffer size (matches factory firmware)
    };

    esp_err_t ret = spi_bus_initialize(BSP_LCD_SPI_NUM, &qspi_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI3 bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    initialized = true;
    return ESP_OK;
}

static esp_err_t bsp_lcd_backlight_init(void)
{
    const ledc_channel_config_t backlight_channel = {
        .gpio_num = BSP_LCD_GPIO_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = DRV_LCD_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = BIT(DRV_LCD_LEDC_DUTY_RES),
        .hpoint = 0,
    };
    const ledc_timer_config_t backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = DRV_LCD_LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    esp_err_t ret = ledc_timer_config(&backlight_timer);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Backlight timer config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = ledc_channel_config(&backlight_channel);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Backlight channel config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = bsp_lcd_brightness_set(0);
    if (ret != ESP_OK)
    {
        return ret;
    }

    return ESP_OK;
}

static esp_err_t bsp_lcd_brightness_set(int brightness_percent)
{
    if (brightness_percent > 100)
    {
        brightness_percent = 100;
    }
    if (brightness_percent < 0)
    {
        brightness_percent = 0;
    }

    uint32_t duty_cycle = (BIT(DRV_LCD_LEDC_DUTY_RES) * (brightness_percent)) / 100;
    esp_err_t ret = ledc_set_duty(LEDC_LOW_SPEED_MODE, DRV_LCD_LEDC_CH, duty_cycle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Backlight set duty failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = ledc_update_duty(LEDC_LOW_SPEED_MODE, DRV_LCD_LEDC_CH);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Backlight update duty failed: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

static esp_err_t bsp_lcd_panel_init(void)
{
    if (panel_handle != NULL && panel_io_handle != NULL)
    {
        return ESP_OK;
    }

    esp_err_t ret = bsp_spi_bus_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = bsp_lcd_backlight_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Backlight init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = BSP_LCD_SPI_CS,
        .dc_gpio_num = -1,
        .spi_mode = 3,
        .pclk_hz = DRV_LCD_PIXEL_CLK_HZ,
        .trans_queue_depth = 2,  // Reduced from 10 to avoid SPI queue errors
        .lcd_cmd_bits = DRV_LCD_CMD_BITS,
        .lcd_param_bits = DRV_LCD_PARAM_BITS,
        .flags = {
            .quad_mode = true,
        },
    };
    spd2010_vendor_config_t vendor_config = {
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &panel_io_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD panel IO init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_GPIO_RST,
        .rgb_ele_order = DRV_LCD_RGB_ELEMENT_ORDER,
        .bits_per_pixel = DRV_LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ret = esp_lcd_new_panel_spd2010(panel_io_handle, &panel_config, &panel_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD panel init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_lcd_panel_reset(panel_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD panel reset failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_lcd_panel_init(panel_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD panel init sequence failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_lcd_panel_mirror(panel_handle, DRV_LCD_MIRROR_X, DRV_LCD_MIRROR_Y);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD mirror failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_lcd_panel_disp_on_off(panel_handle, true);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD display on failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

static lv_disp_t *bsp_lvgl_init(void)
{
    if (lvgl_disp != NULL)
    {
        return lvgl_disp;
    }

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    // Reduce refresh rate to ease SPI/LCD pressure and keep audio stable.
    lvgl_cfg.timer_period_ms = 20;
    lvgl_cfg.task_max_sleep_ms = 100;
    esp_err_t ret = lvgl_port_init(&lvgl_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LVGL port init failed: %s", esp_err_to_name(ret));
        return NULL;
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io_handle,
        .panel_handle = panel_handle,
        .buffer_size = DRV_LCD_H_RES * LVGL_DRAW_BUFF_HEIGHT,
        .double_buffer = LVGL_DRAW_BUFF_DOUBLE,
        .hres = DRV_LCD_H_RES,
        .vres = DRV_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = DRV_LCD_SWAP_XY,
            .mirror_x = DRV_LCD_MIRROR_X,
            .mirror_y = DRV_LCD_MIRROR_Y,
        },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,  // Match factory - use SPIRAM for display buffer (required for screen transitions)
        },
    };

    lvgl_disp = lvgl_port_add_disp(&disp_cfg);
    if (lvgl_disp == NULL)
    {
        ESP_LOGE(TAG, "LVGL display init failed");
    }

    return lvgl_disp;
}

static esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    if (i2s_data_if == NULL)
    {
        bsp_i2c0_bus_init();
        bsp_audio_init();
    }

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_GENERAL_I2C_NUM,
        .addr = (DRV_ES8311_I2C_ADDR << 1),
        .bus_handle = NULL, // Use legacy port
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(i2c_ctrl_if);

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = i2c_ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_NUM_NC,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };
    const audio_codec_if_t *es8311_dev = es8311_codec_new(&es8311_cfg);
    if (es8311_dev == NULL)
    {
        ESP_LOGE(TAG, "Failed to create ES8311 codec");
        return NULL;
    }

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_dev,
        .data_if = i2s_data_if,
    };
    return esp_codec_dev_new(&codec_dev_cfg);
}

// Write a single ES8311 register directly over the (legacy) I2C bus the codec
// already lives on. esp_codec_dev exposes set_in_gain (analog PGA, 0 dB floor)
// and set_in_mute (disables the ADC → kills esp_capture), but NOT the ADC
// digital-volume register (REG17), which is the only way to get a true,
// capture-safe mic mute. Used by media.c::apply_codec_mute for half-duplex
// echo suppression. The legacy i2c driver serializes the bus, so this is safe
// to call concurrently with esp_codec_dev gain writes.
int board_codec_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    esp_err_t ret = i2c_master_write_to_device(BSP_GENERAL_I2C_NUM, DRV_ES8311_I2C_ADDR,
                                               buf, sizeof(buf), pdMS_TO_TICKS(50));
    return (ret == ESP_OK) ? 0 : -1;
}

static esp_err_t bsp_i2c_check(i2c_port_t port, uint8_t address)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (address << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(port, cmd, 100 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    return ret;
}

static void bsp_i2c_scan(i2c_port_t port)
{
    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
    for (int i = 0; i < 128; i += 16)
    {
        printf("%02x: ", i);
        for (int j = 0; j < 16; j++)
        {
            fflush(stdout);
            esp_err_t ret = bsp_i2c_check(port, i + j);
            if (ret == ESP_OK)
            {
                printf("%02x ", i + j);
            }
            else if (ret == ESP_ERR_TIMEOUT)
            {
                printf("TO ");
            }
            else
            {
                printf("-- ");
            }
        }
        printf("\n");
    }
}

static esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void)
{
    if (i2s_data_if == NULL)
    {
        bsp_i2c0_bus_init();
        bsp_audio_init();
    }

    const audio_codec_if_t *es7243_dev = NULL;

    // Check which address responds (0x13 or 0x14) - passing 7-bit address to probe
    // Note: bsp_i2c_check now needs to support legacy check manually if it used bus handle
    // For now we assume header modification or check bsp_i2c_check implementation.
    // Wait, bsp_i2c_check takes bus handle. We need to create a legacy version or pass something else.
    // Let's postpone bsp_i2c_check fix and just try initializing directly or dummy check.

    // Quick fix: Just try initializing 0x13 then 0x14 without check, or assume 0x13 (ES7243)
    // Actually, let's fix bsp_i2c_check separately if it breaks.
    // For this tool call, let's update the codec init.

    if (bsp_i2c_check(i2c0_port, DRV_ES7243_I2C_ADDR) == ESP_OK)
    {
        audio_codec_i2c_cfg_t i2c_cfg = {
            .port = BSP_GENERAL_I2C_NUM,
            .addr = (DRV_ES7243_I2C_ADDR << 1),
            .bus_handle = NULL, // Use legacy port
        };
        const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
        es7243_codec_cfg_t es7243_cfg = { .ctrl_if = i2c_ctrl_if };
        es7243_dev = es7243_codec_new(&es7243_cfg);
    }
    else if (bsp_i2c_check(i2c0_port, DRV_ES7243E_I2C_ADDR) == ESP_OK)
    {
        // Try fallback address
        audio_codec_i2c_cfg_t i2c_cfg = {
            .port = BSP_GENERAL_I2C_NUM,
            .addr = (DRV_ES7243E_I2C_ADDR << 1),
            .bus_handle = NULL,
        };
        const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
        es7243e_codec_cfg_t es7243e_cfg = { .ctrl_if = i2c_ctrl_if };
        es7243_dev = es7243e_codec_new(&es7243e_cfg);
    }

    // assert(es7243_dev); // Removed to prevent crash
    if (es7243_dev == NULL)
    {
        ESP_LOGE(TAG, "Failed to find/create ES7243/E codec");
        return NULL;
    }

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7243_dev,
        .data_if = i2s_data_if,
    };
    return esp_codec_dev_new(&codec_dev_cfg);
}

esp_io_expander_handle_t bsp_io_expander_init(void)
{
    if (io_exp_handle != NULL)
    {
        return io_exp_handle;
    }

    bsp_i2c0_bus_init();

    const pca95xx_16bit_ex_config_t io_exp_config = {
        .int_gpio = BSP_IO_EXPANDER_INT,
        .update_interval_us = 1000000, // 1s
    };

    esp_err_t ret = esp_io_expander_new_i2c_pca95xx_16bit_ex(BSP_GENERAL_I2C_NUM, ESP_IO_EXPANDER_I2C_PCA9535_ADDRESS_001, &io_exp_config, &io_exp_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create IO Expander: %s", esp_err_to_name(ret));
        return NULL;
    }

    uint32_t pin_values = 0;
    esp_io_expander_get_level(io_exp_handle, 0xFFFF, &pin_values);
    ESP_LOGI(TAG, "IO Expander initial levels: 0x%04" PRIx32, pin_values);
    ESP_LOGI(TAG, "IO Expander masks: input=0x%04x output=0x%04x reset=0x%04x", (int)DRV_IO_EXP_INPUT_MASK, (int)DRV_IO_EXP_OUTPUT_MASK, (int)BSP_SSCMA_CLIENT_RST);

    esp_io_expander_set_dir(io_exp_handle, DRV_IO_EXP_INPUT_MASK, IO_EXPANDER_INPUT);
    esp_io_expander_set_dir(io_exp_handle, DRV_IO_EXP_OUTPUT_MASK, IO_EXPANDER_OUTPUT);
    ret = esp_io_expander_set_dir(io_exp_handle, BSP_SSCMA_CLIENT_RST, IO_EXPANDER_OUTPUT);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set reset pin direction (0x%04x): %s", (int)BSP_SSCMA_CLIENT_RST, esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG, "IO Expander: reset pin (0x%04x) forced to OUTPUT", (int)BSP_SSCMA_CLIENT_RST);
    }

    // v23: Enforce absolute parity with sensecap-watcher.c:189-193
    ESP_LOGI(TAG, "Driving all IO Expanders LOW initially...");
    esp_io_expander_set_level(io_exp_handle, DRV_IO_EXP_OUTPUT_MASK, 0);

    esp_io_expander_set_level(io_exp_handle, BSP_PWR_SYSTEM, 1);
    vTaskDelay(pdMS_TO_TICKS(100)); // Core BSP delay

    ESP_LOGI(TAG, "Powering up START_UP rails (mask 0x%04x) and Pin 7 (Reset) via expander", (int)(BSP_PWR_START_UP | BSP_SSCMA_CLIENT_RST));
    esp_io_expander_set_level(io_exp_handle, BSP_PWR_START_UP | BSP_SSCMA_CLIENT_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50)); // Core BSP delay

    esp_io_expander_get_level(io_exp_handle, 0xFFFF, &pin_values);
    ESP_LOGI(TAG, "IO Expander levels after power up: 0x%04" PRIx32, pin_values);

    return io_exp_handle;
}

static esp_err_t bsp_codec_set_fs(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    esp_err_t ret = ESP_OK;
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = rate,
        .channel = ch,
        .bits_per_sample = bits_cfg,
    };

    if (codec_mutex)
        xSemaphoreTake(codec_mutex, portMAX_DELAY);

    if (play_dev_handle)
    {
        ret |= esp_codec_dev_close(play_dev_handle);
        ret |= esp_codec_dev_open(play_dev_handle, &fs);
    }
    if (record_dev_handle)
    {
        ret |= esp_codec_dev_close(record_dev_handle);
        ret |= esp_codec_dev_set_in_gain(record_dev_handle, DRV_AUDIO_MIC_GAIN);
        fs.channel = 2; // ? original code set 2
        fs.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
        ret |= esp_codec_dev_open(record_dev_handle, &fs);
    }

    if (codec_mutex)
        xSemaphoreGive(codec_mutex);
    return ret;
}

// Re-pin the mic to the RIGHT I2S slot on the reconnect JOIN path.
//
// ROOT CAUSE (verified 2026-06-04, I2S-peripheral level):
//   The slot is selected by i2s_channel_reconfig_std_slot() (via the codec
//   data-if set_drv_fs, audio_codec_data_i2s.c). At BOOT, bsp_audio_init pins
//   the RX channel to I2S_STD_SLOT_RIGHT (slot 1 = the only wired ADC slot) and
//   bsp_codec_set_fs opens record with channel=2 + channel_mask=MAKE_CHANNEL_MASK(1),
//   which (a) keeps slot RIGHT and (b) leaves record_dev_handle->input_opened=true.
//   The capturer's first esp_codec_dev_open(channel=1) then hits the "Input
//   already open" no-op (esp_codec_dev.c) and never reaches set_fmt, so it
//   cannot remap the slot. Boot mic works.
//
//   On a DISCONNECT, the LiveKit teardown closes the record codec
//   (input_opened=false). On RECONNECT the capturer's esp_codec_dev_open runs
//   set_fmt for real with channel=1, and _i2s_data_set_fmt UNCONDITIONALLY
//   rewrites a mono (channel==1) open to channel=2 + channel_mask=MAKE_CHANNEL_MASK(0)
//   = slot 0 = LEFT = the UNWIRED, silent slot. Hence "mic captures the silent
//   slot on every reconnect" (micdbg peak ~9-80 instead of thousands).
//
// WHY PRIOR FIXES FAILED:
//   * Setting channel_mask=MAKE_CHANNEL_MASK(1) in audio_dev_src_start while
//     leaving channel=1: the channel==1 branch (audio_codec_data_i2s.c:414-419)
//     fires first and OVERWRITES the mask back to slot 0. No-op. (Failed fix #2,
//     the channel_mask-only variant.)
//   * Forcing channel=2 in audio_dev_src_start: desyncs esp_capture's own
//     enable-state tracking against the duplex codec → "i2s_channel_disable:
//     channel not enabled" + ~11 s reconnect loop. (Failed fix #2, reverted.)
//   * Calling bsp_codec_set_fs AFTER the renderer was live: closed/reopened the
//     record handle while TX was open → RX-disabled-while-TX-open → "AUD_SRC
//     ret -8" dead mic. (Failed fix #1.)
//
// THE FIX — re-pin at the I2S-peripheral layer, the layer that actually decides
// the slot, then restore the boot codec invariant (input_opened=true), WITHOUT
// closing the codec under a live TX:
//
//   Step 1 (peripheral): disable the RX channel (i2s_channel_reconfig_std_slot
//     requires READY/disabled state), re-apply the boot slot config
//     (I2S_STD_SLOT_RIGHT), then re-enable it. This is byte-for-byte the boot
//     sequence (board.c bsp_audio_init), so the slot is hardware-pinned RIGHT
//     again regardless of what the prior session left behind.
//   Step 2 (codec): reopen record_dev_handle the boot way (channel=2 +
//     MAKE_CHANNEL_MASK(1)) so input_opened=true and the capturer's later
//     channel=1 open hits the "Input already open" no-op and CANNOT remap to
//     slot 0. We FIRST i2s_channel_enable() both channels (tolerating the
//     benign "already enabled" return) so the codec's internal disable/enable
//     in esp_codec_dev_open is valid from a known-enabled state, exactly like
//     boot — this is what prevents the "i2s_channel_disable: channel not
//     enabled" desync that bit the earlier reopen attempts.
//
// CRASH-SAFE on the reconnect JOIN path ONLY:
//   - Runs after leave_room (close→settle→destroy) and media_cleanup, so NO
//     peer_task / capture thread references the codec (avoids the leave_room
//     subscribe-path panic).
//   - The play (TX) codec handle is CLOSED at this point: av_render defers the
//     play esp_codec_dev_open to the FIRST agent audio packet (av_render.c:957),
//     and media_cleanup→av_render_close already closed it. So this never
//     disables RX while TX is actively streaming → no "AUD_SRC ret -8".
//   - It does NOT touch esp_capture / audio_dev_src state, so esp_capture's
//     enable-state bookkeeping never desyncs.
//   MUST be called BETWEEN media_init() and livekit_room_connect() on reconnect.
void board_codec_reinit_record(void)
{
    // Step 1: re-apply the boot-time RIGHT slot directly at the i2s_std layer.
    if (i2s_rx_chan != NULL) {
        // i2s_channel_reconfig_std_slot() requires the channel be disabled
        // (I2S_CHAN_STATE_READY). Disabling an already-disabled channel returns
        // a benign ESP_ERR_INVALID_STATE — ignore it; we only care that the
        // channel ends up disabled before the reconfig.
        i2s_channel_disable(i2s_rx_chan);

        i2s_std_config_t std_cfg = BSP_I2S_DUPLEX_MONO_CFG(DRV_AUDIO_SAMPLE_RATE);
        std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;  // slot 1 = the wired ES7243E ADC
        esp_err_t slot_ret = i2s_channel_reconfig_std_slot(i2s_rx_chan, &std_cfg.slot_cfg);
        if (slot_ret != ESP_OK) {
            ESP_LOGW(TAG, "board_codec_reinit_record: RX slot reconfig failed (0x%x)", slot_ret);
        }
        // Re-enable RX so the codec reopen below starts from the boot-equivalent
        // enabled state. "already enabled" is benign — ignore.
        i2s_channel_enable(i2s_rx_chan);
    }
    // TX must also be enabled going into the codec reopen so esp_codec_dev_open's
    // internal disable/enable on the OUT path is valid (boot-equivalent). Benign
    // if already enabled.
    if (i2s_tx_chan != NULL) {
        i2s_channel_enable(i2s_tx_chan);
    }

    // Step 2: restore the boot codec invariant — record opened with channel=2 +
    // MAKE_CHANNEL_MASK(1) leaves input_opened=true, so the capturer's later
    // channel=1 open no-ops and cannot remap the slot back to LEFT.
    esp_err_t ret = bsp_codec_set_fs(DRV_AUDIO_SAMPLE_RATE, DRV_AUDIO_SAMPLE_BITS, DRV_AUDIO_CHANNELS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "board_codec_reinit_record: bsp_codec_set_fs failed (0x%x)", ret);
    } else {
        ESP_LOGI(TAG, "board_codec_reinit_record: re-pinned mic to RIGHT I2S slot (slot 1)");
    }
}

esp_lcd_touch_handle_t bsp_touch_init(void)
{
    if (touch_handle != NULL)
    {
        return touch_handle;
    }

    bsp_i2c1_bus_init();

    const esp_lcd_touch_config_t touch_config = {
        .x_max = DRV_LCD_H_RES,
        .y_max = DRV_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = DRV_LCD_SWAP_XY,
            .mirror_x = DRV_LCD_MIRROR_X,
            .mirror_y = DRV_LCD_MIRROR_Y,
        },
        .user_data = NULL,
    };

    ESP_LOGI(TAG, "Initializing SPD2010 touch IO");
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    const esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_SPD2010_CONFIG();
    if (esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)BSP_TOUCH_I2C_NUM, &tp_io_config, &tp_io_handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create touch IO");
        return NULL;
    }

    if (esp_lcd_touch_new_i2c_spd2010(tp_io_handle, &touch_config, &touch_handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize SPD2010 touch");
        return NULL;
    }

    // Match factory firmware: read touch data once to initialize the panel
    ESP_LOGI(TAG, "Touch panel hardware initialized, performing initial read");
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_lcd_touch_read_data(touch_handle);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Touch panel initialized successfully");
    return touch_handle;
}

/**
 * @brief Register touch panel with LVGL as input device
 *
 * This MUST be called after both bsp_lvgl_init() and bsp_touch_init()
 * to enable touch input for LVGL widgets.
 */
static lv_indev_t *bsp_touch_indev_init(void)
{
    if (lvgl_touch_indev != NULL)
    {
        return lvgl_touch_indev;
    }

    if (lvgl_disp == NULL)
    {
        ESP_LOGE(TAG, "Cannot init touch indev: LVGL display not initialized");
        return NULL;
    }

    if (touch_handle == NULL)
    {
        ESP_LOGE(TAG, "Cannot init touch indev: Touch panel not initialized");
        return NULL;
    }

    ESP_LOGI(TAG, "Registering touch panel with LVGL...");

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = lvgl_disp,
        .handle = touch_handle,
        .sensitivity = 1,  // Default sensitivity
    };

    lvgl_touch_indev = lvgl_port_add_touch(&touch_cfg);
    if (lvgl_touch_indev == NULL)
    {
        ESP_LOGE(TAG, "Failed to register touch with LVGL");
        return NULL;
    }

    ESP_LOGI(TAG, "Touch input device registered with LVGL successfully");
    return lvgl_touch_indev;
}

// Public functions

void board_init()
{
    ESP_LOGI(TAG, "Initializing board (Watcher Modernized BSP)");

    // 1. Initialize IO Expander and Power on peripherals
    io_exp_handle = bsp_io_expander_init();
    if (io_exp_handle == NULL)
    {
        ESP_LOGE(TAG, "Critical: IO Expander init failed");
    }

    // Spurious EXT0 wake recovery.
    //
    // The IO expander INT line is the only EXT0 wake source on this board.
    // It asserts on ANY input-pin change — knob press AND charge controller
    // events (CHRG_DET, STDBY_DET, VBUS_IN_DET, BAT_DET). The PCA9535 has
    // no per-pin interrupt mask, so we can't filter at the chip; we have
    // to detect spurious wakes at boot and immediately re-sleep.
    //
    // Verified 2026-05-21 via serial log: device woke from deep sleep with
    // `wake_cause=EXT0` despite the IO expander INT line having been
    // continuously HIGH for 200ms straight on both digital and RTC
    // subsystems immediately before esp_deep_sleep_start(). Means the
    // wake transition happened AFTER sleep entry — almost certainly the
    // charge controller doing a top-off cycle while battery is full on USB.
    //
    // This check must run AFTER io_exp_handle init but BEFORE LCD/LVGL/etc.
    // so the user sees no display flash on a spurious wake — to them it
    // looks like the device stayed asleep.
    if (io_exp_handle != NULL && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0)
    {
        // Poll the knob briefly. The EXT0 wake might have been caused by
        // a legitimate knob press AND the user might have released by
        // the time we get here. Without polling we'd resleep on every
        // tap shorter than ~100ms. With 100ms of polling we catch most
        // human button presses.
        bool knob_seen_pressed = false;
        for (int i = 0; i < 10; i++)
        {
            if (board_is_knob_pressed())
            {
                knob_seen_pressed = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        // Dump state for diagnostics even if we proceed with boot.
        uint32_t input_state = 0;
        esp_io_expander_get_level(io_exp_handle, 0xFFFF, &input_state);
        ESP_LOGI(TAG, "EXT0 wake recovery: knob_seen_pressed=%d io_input=0x%04lx "
                 "(chrg=%lu stdby=%lu vbus=%lu knob=%lu bat=%lu)",
                 knob_seen_pressed,
                 (unsigned long)(input_state & 0xFFFF),
                 (unsigned long)((input_state >> 0)  & 1),
                 (unsigned long)((input_state >> 1)  & 1),
                 (unsigned long)((input_state >> 2)  & 1),
                 (unsigned long)((input_state >> 3)  & 1),
                 (unsigned long)((input_state >> 11) & 1));

        if (!knob_seen_pressed)
        {
            ESP_LOGI(TAG, "Spurious EXT0 wake (no knob press detected) - re-entering deep sleep");
            bsp_system_deep_sleep(0);
            // bsp_system_deep_sleep() does not return. If it ever does
            // (future variant, sleep entry failure), don't fall through
            // into a half-initialized boot with no LCD/touch/audio — log
            // and restart cleanly.
            ESP_LOGE(TAG, "bsp_system_deep_sleep returned unexpectedly - restarting");
            esp_restart();
        }

        ESP_LOGI(TAG, "EXT0 wake confirmed real (knob press) - proceeding with boot");
    }

    ESP_LOGI(TAG, "Initializing LCD panel before touch...");
    if (bsp_lcd_panel_init() == ESP_OK)
    {
        ESP_LOGI(TAG, "LCD panel initialized, enabling backlight");
        bsp_lcd_brightness_set(100);
        if (bsp_lvgl_init() != NULL)
        {
            ui_init();
            ui_listening();
            ESP_LOGI(TAG, "LVGL UI initialized");
        }
        else
        {
            ESP_LOGE(TAG, "LVGL init failed, UI not available");
        }
    }
    else
    {
        ESP_LOGE(TAG, "LCD panel init failed, backlight stays off");
    }

    // 0. Scan I2C0 & I2C1 Bus for diagnostics (After power up)
    printf("Scanning I2C0 Bus (after power-up):\n");
    bsp_i2c_scan(BSP_GENERAL_I2C_NUM);
    bsp_i2c1_bus_init();
    printf("Scanning I2C1 Bus:\n");
    bsp_i2c_scan(BSP_TOUCH_I2C_NUM);

    // 2. Initialize Audio
    codec_mutex = xSemaphoreCreateMutex();

    play_dev_handle = bsp_audio_codec_speaker_init();
    record_dev_handle = bsp_audio_codec_microphone_init();

    if (play_dev_handle && record_dev_handle)
    {
        bsp_codec_set_fs(DRV_AUDIO_SAMPLE_RATE, DRV_AUDIO_SAMPLE_BITS, DRV_AUDIO_CHANNELS);
        // Set default volume
        esp_codec_dev_set_out_vol(play_dev_handle, CONFIG_LK_EXAMPLE_SPEAKER_VOLUME);
    }

    // 3. Initialize Touch hardware
    bsp_touch_init();

    // 4. Register Touch with LVGL (CRITICAL: must be after LVGL and touch hardware init)
    if (lvgl_disp != NULL && touch_handle != NULL)
    {
        if (bsp_touch_indev_init() != NULL)
        {
            ESP_LOGI(TAG, "Touch input enabled for LVGL UI");
        }
        else
        {
            ESP_LOGW(TAG, "Touch input NOT enabled - buttons will not respond to touch");
        }
    }
    else
    {
        ESP_LOGW(TAG, "Skipping touch LVGL registration (disp=%p, touch=%p)", lvgl_disp, touch_handle);
    }

    ESP_LOGI(TAG, "Board initialization complete");
}

float board_get_temp(void)
{
    return 0.0f;
}

esp_codec_dev_handle_t get_record_handle(void)
{
    return record_dev_handle;
}

esp_codec_dev_handle_t get_playback_handle(void)
{
    return play_dev_handle;
}

bool board_is_knob_pressed(void)
{
    if (io_exp_handle == NULL)
    {
        return false;
    }

    uint32_t pin_values = 0;
    esp_err_t err = esp_io_expander_get_level(io_exp_handle, BSP_KNOB_BTN, &pin_values);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to read knob button: %s", esp_err_to_name(err));
        return false;
    }

    // Button is active-low on the expander.
    return (pin_values & BSP_KNOB_BTN) == 0;
}

// ---- Battery / charge state -----------------------------------------------
// The factory BSP (components/sensecap-watcher) provides bsp_battery_* and
// bsp_system_is_charging, but this example links its own board.c instead of
// that component, so the few readers the status-bar UI needs are reimplemented
// here using the same ADC path + curve-fit and the same PCA9535 bits the BSP
// uses. Safe to call from the LVGL timer task (single ADC consumer; the
// expander I2C reads are serialized by the ESP-IDF I2C driver).

static uint16_t board_battery_get_voltage_mv(void)
{
    static bool adc_ready = false;
    static adc_oneshot_unit_handle_t adc_handle = NULL;
    static adc_cali_handle_t cali_handle = NULL;

    if (!adc_ready) {
        adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = ADC_UNIT_1 };
        if (adc_oneshot_new_unit(&init_cfg, &adc_handle) != ESP_OK) {
            return 0;
        }
        adc_oneshot_chan_cfg_t ch_cfg = {
            .bitwidth = ADC_BITWIDTH_DEFAULT,
            .atten = BSP_BAT_ADC_ATTEN,
        };
        adc_oneshot_config_channel(adc_handle, BSP_BAT_ADC_CHAN, &ch_cfg);
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = ADC_UNIT_1,
            .chan = BSP_BAT_ADC_CHAN,
            .atten = BSP_BAT_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_handle) != ESP_OK) {
            return 0;
        }
        adc_ready = true;
    }

    int raw = 0, mv = 0;
    if (adc_oneshot_read(adc_handle, BSP_BAT_ADC_CHAN, &raw) != ESP_OK) {
        return 0;
    }
    adc_cali_raw_to_voltage(cali_handle, raw, &mv);
    mv = mv * 82 / 20; // undo the 4.1x divider (62k+20k / 20k)
    return (uint16_t)mv;
}

uint8_t board_get_battery_percent(void)
{
    int32_t mv = 0;
    for (uint8_t i = 0; i < 10; i++) {
        mv += board_battery_get_voltage_mv();
    }
    mv /= 10;
    // Quadratic voltage→% curve-fit (from the SenseCAP BSP), tuned for this
    // cell chemistry. Clamp to [0,100].
    int pct = (int)((-1 * mv * mv + 9016 * mv - 19189000) / 10000);
    if (pct > 100) pct = 100;
    if (pct < 0)   pct = 0;
    return (uint8_t)pct;
}

bool board_is_charging(void)
{
    if (io_exp_handle == NULL) {
        return false;
    }
    uint32_t pin_values = 0;
    // VBUS_IN_DET, not CHRG_DET: CHRG_DET reads HIGH both when full-on-charger
    // and when unplugged, so the bolt never cleared on unplug. VBUS_IN_DET is
    // active-low — LOW = USB plugged in (verified: expander 0xffd9, bit2=0 on
    // USB). So "on external power" = VBUS line low.
    if (esp_io_expander_get_level(io_exp_handle, BSP_PWR_VBUS_IN_DET, &pin_values) != ESP_OK) {
        return false;
    }
    return (pin_values & BSP_PWR_VBUS_IN_DET) == 0; // LOW = USB present
}

bool board_is_battery_present(void)
{
    if (io_exp_handle == NULL) {
        return true; // unknown → assume present so a reading still shows
    }
    uint32_t pin_values = 0;
    if (esp_io_expander_get_level(io_exp_handle, BSP_PWR_BAT_DET, &pin_values) != ESP_OK) {
        return true;
    }
    // Active-low: BAT_DET low = battery present (mirrors bsp_battery_is_present).
    return (pin_values & BSP_PWR_BAT_DET) == 0;
}

void bsp_system_deep_sleep(uint32_t time_in_sec)
{
    ESP_LOGI(TAG, "Preparing for deep sleep...");

    // Wait for button to be fully released and stable.
    // This prevents immediate wake from the button release interrupt.
    // Capped at ~3 s wall-clock so a stuck-low knob input (hardware fault,
    // debris, expander stuck) can't hang the entire sleep path forever —
    // we proceed to sleep anyway after the ceiling and log it.
    ESP_LOGI(TAG, "Waiting for button release to stabilize...");
    int stable_count = 0;
    int release_attempts = 0;
    const int RELEASE_MAX_ATTEMPTS = 60; // 60 * 50 ms = 3 s
    while (stable_count < 10 && release_attempts < RELEASE_MAX_ATTEMPTS)  // 10 consecutive "not pressed"
    {
        if (board_is_knob_pressed())
        {
            stable_count = 0;  // Reset if still pressed
        }
        else
        {
            stable_count++;
        }
        vTaskDelay(pdMS_TO_TICKS(50));  // Check every 50ms
        release_attempts++;
    }
    if (stable_count < 10)
    {
        ESP_LOGW(TAG, "Button release never stabilized (%d attempts) - knob may be stuck; sleeping anyway",
                 release_attempts);
    }
    else
    {
        ESP_LOGI(TAG, "Button released and stable");
    }

    if (time_in_sec > 0)
    {
        esp_sleep_enable_timer_wakeup(time_in_sec * 1000000ULL);
    }

    // Turn off peripherals to save power. This MUST happen BEFORE the
    // final INT-stabilization loop — cutting power to SDCARD / LCD /
    // CODEC_PA / etc. causes transitions on the IO expander's INPUT
    // pins (CHRG_DET, STDBY_DET, VBUS_IN_DET, BAT_DET), each of which
    // latches the open-drain INT line LOW. EXT0 wake is level-triggered,
    // so any latched LOW at esp_deep_sleep_start() time = immediate wake
    // = device reboots right after "Goodbye" instead of sleeping.
    // (Previous version ran the INT-stabilization loop BEFORE this
    // power-down, so the cleared INT got re-latched by the power-down
    // itself and we slept with INT low. Verified 2026-05-21 via serial
    // log: reset_reason=DEEPSLEEP wake_cause=EXT0 within ~ms of sleep.)
    uint32_t pin_mask_sleep = BSP_PWR_SDCARD | BSP_PWR_CODEC_PA | BSP_PWR_GROVE | BSP_PWR_BAT_ADC | BSP_PWR_LCD | BSP_PWR_AI_CHIP;
    if (io_exp_handle != NULL)
    {
        esp_io_expander_set_level(io_exp_handle, pin_mask_sleep, 0);
    }

    // Let peripheral power-down transitions settle before flushing INT.
    vTaskDelay(pdMS_TO_TICKS(100));

    // Wait for the INT line to be CONTINUOUSLY HIGH for STABLE_MS straight,
    // not just instantaneously HIGH on one check. A single check is fooled
    // by anything re-asserting INT at sub-100 ms cadence — verified
    // 2026-05-21 with `attempts=0` (instantaneous HIGH) followed by EXT0
    // wake the moment esp_deep_sleep_start() ran 11 ms later.
    //
    // Most likely re-asserters on a USB-tethered test setup: the charge
    // controller cycling CHRG_DET / STDBY_DET on the IO expander's input
    // pins as it negotiates with the battery. The PCA9535 has no per-pin
    // interrupt mask, so the only option is to wait for the inputs to
    // actually go quiet.
    //
    // Algorithm: flush INT, then watch the GPIO line for STABLE_MS. If
    // it stays HIGH the whole window we're done. If it goes LOW, count
    // the event, flush again, repeat. Give up after MAX_WAIT_MS — at
    // that point sleep is risky but trying forever is worse.
    ESP_LOGI(TAG, "Waiting for INT line to be continuously HIGH for 200ms...");
    const int STABLE_MS   = 200;   // INT must stay HIGH this long, uninterrupted
    const int POLL_MS     = 10;    // line-level sample interval
    const int MAX_WAIT_MS = 5000;  // overall ceiling

    TickType_t start_ticks = xTaskGetTickCount();
    int flush_count    = 0;
    int re_assert_count = 0;
    bool sleep_safe = false;

    while (1)
    {
        if (io_exp_handle != NULL)
        {
            uint32_t dummy;
            esp_io_expander_get_level(io_exp_handle, 0xFFFF, &dummy);
            flush_count++;
        }

        // Watch for STABLE_MS straight; bail out the moment INT drops.
        bool dropped = false;
        int samples = STABLE_MS / POLL_MS;
        for (int i = 0; i < samples; i++)
        {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            if (gpio_get_level(BSP_IO_EXPANDER_INT) == 0)
            {
                dropped = true;
                re_assert_count++;
                break;
            }
        }

        if (!dropped)
        {
            sleep_safe = true;
            break;
        }

        uint32_t elapsed_ms = (xTaskGetTickCount() - start_ticks) * portTICK_PERIOD_MS;
        if (elapsed_ms >= MAX_WAIT_MS)
        {
            break;
        }
    }

    uint32_t total_wait_ms = (xTaskGetTickCount() - start_ticks) * portTICK_PERIOD_MS;
    if (sleep_safe)
    {
        ESP_LOGI(TAG, "INT stable HIGH (flushes=%d, re-asserts=%d, total=%lums) - ready for sleep",
                 flush_count, re_assert_count, (unsigned long)total_wait_ms);
    }
    else
    {
        ESP_LOGW(TAG, "INT never settled after %lums (flushes=%d, re-asserts=%d) - sleep may wake immediately. "
                      "Unplug USB if testing on-bench; the charge controller is likely fluttering "
                      "CHRG_DET / STDBY_DET on the IO expander.",
                 (unsigned long)total_wait_ms, flush_count, re_assert_count);
    }

    // Dump the IO expander input register state. If a specific INPUT pin
    // is "stuck" LOW (e.g. CHRG_DET active while battery charging on USB),
    // we'll see it here — that tells us if the latched INT is being driven
    // by a real, persistent input condition vs. transient noise.
    if (io_exp_handle != NULL)
    {
        uint32_t input_state = 0;
        esp_io_expander_get_level(io_exp_handle, 0xFFFF, &input_state);
        // Active-low signals on inputs: a 0 bit means the corresponding
        // detect line is asserted. Bits we care about:
        //   bit 0 = CHRG_DET (low = charging)
        //   bit 1 = STDBY_DET (low = battery full / standby)
        //   bit 2 = VBUS_IN_DET (low = USB plugged in)
        //   bit 3 = KNOB_BTN (low = pressed)
        //   bit 11 = BAT_DET (low = battery present)
        ESP_LOGI(TAG, "IO expander input register: 0x%04lx "
                 "(chrg=%lu stdby=%lu vbus=%lu knob=%lu bat=%lu)",
                 (unsigned long)(input_state & 0xFFFF),
                 (unsigned long)((input_state >> 0)  & 1),
                 (unsigned long)((input_state >> 1)  & 1),
                 (unsigned long)((input_state >> 2)  & 1),
                 (unsigned long)((input_state >> 3)  & 1),
                 (unsigned long)((input_state >> 11) & 1));
    }

    // Explicitly switch GPIO_NUM_2 (BSP_IO_EXPANDER_INT) from the digital
    // GPIO subsystem to the RTC GPIO subsystem with input + pull-up
    // BEFORE configuring EXT0 wake. Doing this through rtc_gpio_init lets
    // us verify the line level via rtc_gpio_get_level — the digital
    // gpio_get_level() reading is from the digital subsystem and may not
    // reflect what the RTC sampler will see at sleep entry. (Previous
    // attempt: `INT stable HIGH (flushes=1, re-asserts=0)` was reported,
    // but EXT0 fired the wake 1 ms after esp_deep_sleep_start anyway —
    // indicating the digital-vs-RTC level read disagreed.)
    rtc_gpio_init(BSP_IO_EXPANDER_INT);
    rtc_gpio_set_direction(BSP_IO_EXPANDER_INT, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(BSP_IO_EXPANDER_INT);
    rtc_gpio_pulldown_dis(BSP_IO_EXPANDER_INT);

    // Let the RTC pull-up settle, then verify the level via the RTC
    // subsystem. If RTC sees LOW here even though digital saw HIGH for
    // 200 ms, the issue is the mode switch, not a noisy input. Try a
    // few flushes if RTC reads LOW.
    vTaskDelay(pdMS_TO_TICKS(20));
    int rtc_level = rtc_gpio_get_level(BSP_IO_EXPANDER_INT);
    int rtc_flush_count = 0;
    while (rtc_level == 0 && rtc_flush_count < 20)
    {
        if (io_exp_handle != NULL)
        {
            uint32_t dummy;
            esp_io_expander_get_level(io_exp_handle, 0xFFFF, &dummy);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        rtc_level = rtc_gpio_get_level(BSP_IO_EXPANDER_INT);
        rtc_flush_count++;
    }
    int digital_level = gpio_get_level(BSP_IO_EXPANDER_INT);
    ESP_LOGI(TAG, "Pre-sleep pin state: rtc_level=%d digital_level=%d (rtc_flushes=%d)",
             rtc_level, digital_level, rtc_flush_count);

    // Configure EXT0 wake source — RTC GPIO mode is now already set above.
    esp_sleep_enable_ext0_wakeup(BSP_IO_EXPANDER_INT, 0);

    ESP_LOGI(TAG, "Entering deep sleep now...");
    esp_deep_sleep_start();
}

void bsp_system_reboot(void)
{
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();
}

void bsp_system_shutdown(void)
{
    ESP_LOGI(TAG, "Shutting down...");

    // Cut system power via IO expander
    if (io_exp_handle != NULL)
    {
        esp_io_expander_set_level(io_exp_handle, BSP_PWR_SYSTEM, 0);
    }
    else
    {
        // Fallback to deep sleep if IO expander not available
        ESP_LOGW(TAG, "IO expander not available, falling back to deep sleep");
        bsp_system_deep_sleep(0);
    }
}

//=============================================================================
// Touch Debug Logging
//=============================================================================
static bool touch_debug_enabled = false;
static TaskHandle_t touch_debug_task_handle = NULL;

static void touch_debug_task(void *arg)
{
    ESP_LOGI(TAG, "Touch debug task started - will log touch coordinates");
    uint16_t x[5], y[5];
    uint8_t touch_cnt = 0;
    uint16_t strength[5];
    uint8_t point_id[5];

    while (touch_debug_enabled)
    {
        if (touch_handle != NULL)
        {
            if (esp_lcd_touch_read_data(touch_handle) == ESP_OK)
            {
                if (esp_lcd_touch_get_coordinates(touch_handle, x, y, strength, &touch_cnt, 5) == ESP_OK && touch_cnt > 0)
                {
                    for (int i = 0; i < touch_cnt; i++)
                    {
                        ESP_LOGI(TAG, "TOUCH[%d]: x=%d, y=%d, strength=%d", i, x[i], y[i], strength[i]);
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));  // Poll every 50ms
    }

    ESP_LOGI(TAG, "Touch debug task stopped");
    touch_debug_task_handle = NULL;
    vTaskDelete(NULL);
}

void bsp_touch_debug_enable(bool enable)
{
    if (enable && !touch_debug_enabled)
    {
        touch_debug_enabled = true;
        xTaskCreate(touch_debug_task, "touch_dbg", 4096, NULL, 5, &touch_debug_task_handle);
        ESP_LOGI(TAG, "Touch debug logging ENABLED - touch coordinates will appear in logs");
    }
    else if (!enable && touch_debug_enabled)
    {
        touch_debug_enabled = false;
        ESP_LOGI(TAG, "Touch debug logging DISABLED");
        // Task will self-delete when it sees touch_debug_enabled = false
    }
}

bool bsp_touch_debug_is_enabled(void)
{
    return touch_debug_enabled;
}

lv_indev_t *bsp_get_touch_indev(void)
{
    return lvgl_touch_indev;
}
