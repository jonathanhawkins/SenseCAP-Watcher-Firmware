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
#define DRV_AUDIO_MIC_GAIN    (27.0)
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
#define CONFIG_LVGL_DRAW_BUFF_HEIGHT (412)
#endif
#define LVGL_DRAW_BUFF_HEIGHT (CONFIG_LVGL_DRAW_BUFF_HEIGHT)
#define LVGL_DRAW_BUFF_DOUBLE (0)

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
        .max_transfer_sz = DRV_LCD_H_RES * DRV_LCD_V_RES * DRV_LCD_BITS_PER_PIXEL / 8,
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
            .buff_spiram = false,
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

esp_lcd_touch_handle_t bsp_touch_init(void)
{
    static esp_lcd_touch_handle_t touch_handle = NULL;
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

    ESP_LOGI(TAG, "Touch panel initialized, skipping initial read");
    // Skip initial touch read - was causing crash into download mode
    // vTaskDelay(pdMS_TO_TICKS(50));
    // esp_lcd_touch_read_data(touch_handle);
    // vTaskDelay(pdMS_TO_TICKS(100));

    return touch_handle;
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

    // 3. Initialize Touch (Optional for startup, but nice to have ready)
    bsp_touch_init();

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
    if (play_dev_handle == NULL)
    {
        ESP_LOGW(TAG, "Playback handle missing, attempting re-init");
        play_dev_handle = bsp_audio_codec_speaker_init();
        if (play_dev_handle)
        {
            esp_codec_dev_set_out_vol(play_dev_handle, CONFIG_LK_EXAMPLE_SPEAKER_VOLUME);
        }
    }
    return play_dev_handle;
}
