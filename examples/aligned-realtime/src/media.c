#include "esp_check.h"
#include "esp_log.h"
#include "board.h"
#include "av_render.h"
#include "av_render_default.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_enc_default.h"
#include "esp_capture_defaults.h"
#include "esp_capture_sink.h"
#include "esp_capture_audio_dev_src.h"
#include "port/media_lib_adapter.h"

#include "media.h"

static const char *TAG = "media";

#define NULL_CHECK(pointer, message) ESP_RETURN_ON_FALSE(pointer != NULL, -1, TAG, message)

typedef struct
{
    esp_capture_sink_handle_t capturer_handle;
    esp_capture_audio_src_if_t *audio_source;
} capture_system_t;

typedef struct
{
    audio_render_handle_t audio_renderer;
    av_render_handle_t av_renderer_handle;
} renderer_system_t;

static capture_system_t capturer_system;
static renderer_system_t renderer_system;

static int build_capturer_system(void)
{
    ESP_LOGI(TAG, "Building capturer system...");
    esp_codec_dev_handle_t record_handle = get_record_handle();
    NULL_CHECK(record_handle, "Failed to get record handle");

    // Use raw device capture to keep a 48 kHz mic stream (no wake word/AEC).
    esp_capture_audio_dev_src_cfg_t codec_cfg = { .record_handle = record_handle };
    capturer_system.audio_source = esp_capture_new_audio_dev_src(&codec_cfg);
    NULL_CHECK(capturer_system.audio_source, "Failed to create audio source");

    esp_capture_cfg_t cfg = { .sync_mode = ESP_CAPTURE_SYNC_MODE_AUDIO, .audio_src = capturer_system.audio_source };
    esp_capture_open(&cfg, &capturer_system.capturer_handle);
    NULL_CHECK(capturer_system.capturer_handle, "Failed to open capture system");
    ESP_LOGI(TAG, "Capturer system initialized successfully");
    return 0;
}

static int build_renderer_system(void)
{
    ESP_LOGI(TAG, "Building renderer system...");
    esp_codec_dev_handle_t render_device = get_playback_handle();
    ESP_LOGI(TAG, "DEBUG: get_playback_handle() returned: %p", (void*)render_device);
    NULL_CHECK(render_device, "Failed to get render device handle");

    i2s_render_cfg_t i2s_cfg = { .play_handle = render_device };
    ESP_LOGI(TAG, "DEBUG: i2s_cfg.play_handle = %p, sizeof(i2s_render_cfg_t) = %d",
             (void*)i2s_cfg.play_handle, (int)sizeof(i2s_render_cfg_t));
    renderer_system.audio_renderer = av_render_alloc_i2s_render(&i2s_cfg);
    ESP_LOGI(TAG, "DEBUG: av_render_alloc_i2s_render returned: %p", (void*)renderer_system.audio_renderer);
    NULL_CHECK(renderer_system.audio_renderer, "Failed to create I2S renderer");

    // Initial speaker volume is owned by volume_control_init() after board init.

    // Smaller buffers reduce playback latency; allow drop to avoid "slow motion" when backlogged.
    av_render_cfg_t render_cfg = {
        .audio_render = renderer_system.audio_renderer,
        .audio_raw_fifo_size = 2 * 4096,      // Reduced from 4*4096 to minimize latency
        .audio_render_fifo_size = 16 * 1024,  // Reduced from 32*1024 to prevent buffer overflow
        .allow_drop_data = true,
    };
    renderer_system.av_renderer_handle = av_render_open(&render_cfg);
    NULL_CHECK(renderer_system.av_renderer_handle, "Failed to create AV renderer");

    av_render_audio_frame_info_t frame_info = {
        .sample_rate = 48000,
        .channel = 1,  // FIXED: Mono to match incoming audio stream (was 2)
        .bits_per_sample = 16,
    };
    av_render_set_fixed_frame_info(renderer_system.av_renderer_handle, &frame_info);

    ESP_LOGI(TAG, "Renderer configured: %d Hz, %d ch, %d bits",
             (int)frame_info.sample_rate, (int)frame_info.channel, (int)frame_info.bits_per_sample);
    ESP_LOGI(TAG, "Audio buffers: raw=%d bytes, render=%d bytes",
             (int)render_cfg.audio_raw_fifo_size, (int)render_cfg.audio_render_fifo_size);

    return 0;
}

int media_init(void)
{
    // Register default media library adapter (memory allocation, etc.)
    // CRITICAL: Must be called before any media_lib_* functions!
    esp_err_t ret = media_lib_add_default_adapter();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register media lib adapter: %d", ret);
        return -1;
    }
    ESP_LOGI(TAG, "Media library adapter registered successfully");

    // Register default audio encoder and decoder
    esp_audio_enc_register_default();
    esp_audio_dec_register_default();

    // Build capturer and renderer systems
    build_capturer_system();
    build_renderer_system();
    return 0;
}

esp_capture_handle_t media_get_capturer(void)
{
    return capturer_system.capturer_handle;
}

av_render_handle_t media_get_renderer(void)
{
    return renderer_system.av_renderer_handle;
}

void media_stop(void)
{
    ESP_LOGI(TAG, "Stopping media systems...");

    // Stop the capturer
    if (capturer_system.capturer_handle != NULL) {
        ESP_LOGI(TAG, "Stopping capturer...");
        esp_capture_stop(capturer_system.capturer_handle);
    }

    // Pause and flush the renderer (no av_render_stop exists)
    if (renderer_system.av_renderer_handle != NULL) {
        ESP_LOGI(TAG, "Pausing and flushing renderer...");
        av_render_pause(renderer_system.av_renderer_handle, true);
        av_render_flush(renderer_system.av_renderer_handle);
    }

    ESP_LOGI(TAG, "Media systems stopped");
}

int media_reset(void)
{
    ESP_LOGI(TAG, "Resetting media systems for reconnection...");

    // Close and reopen the capturer
    if (capturer_system.capturer_handle != NULL) {
        ESP_LOGI(TAG, "Closing capturer...");
        esp_capture_close(capturer_system.capturer_handle);
        capturer_system.capturer_handle = NULL;
    }

    // Close and reopen the renderer
    if (renderer_system.av_renderer_handle != NULL) {
        ESP_LOGI(TAG, "Closing renderer...");
        av_render_close(renderer_system.av_renderer_handle);
        renderer_system.av_renderer_handle = NULL;
    }

    // Rebuild the systems
    if (build_capturer_system() != 0) {
        ESP_LOGE(TAG, "Failed to rebuild capturer system");
        return -1;
    }

    if (build_renderer_system() != 0) {
        ESP_LOGE(TAG, "Failed to rebuild renderer system");
        return -1;
    }

    ESP_LOGI(TAG, "Media systems reset successfully");
    return 0;
}
