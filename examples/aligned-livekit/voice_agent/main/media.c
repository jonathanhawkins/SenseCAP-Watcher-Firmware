#include "esp_check.h"
#include "esp_log.h"
#include "board.h"
#include "av_render_default.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_enc_default.h"
#include "esp_capture_defaults.h"
#include "esp_capture_sink.h"
#include "esp_capture_audio_dev_src.h"
#include "esp_codec_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

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

    // Use raw device capture — AEC source was tried (2026-05-18) but caused
    // LVGL LCD flush to OOM (`ESP_ERR_NO_MEM` on the SPI DMA queue) ~4 s
    // after CONNECTED, freezing the UI on "Connecting…". The AFE pipeline
    // pulls in ~50-100 KB of internal SRAM which combined with WebRTC + LVGL
    // strip buffers + audio FIFOs leaves no room for the SPI DMA queue to
    // grow during render. Echo loop returns without AEC — to be re-addressed
    // either by forcing AEC allocations into PSRAM or by half-duplexing the
    // mic while the agent is speaking. See serial log evidence in
    // .claude/rules/watcher-livekit-teardown.md.
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
    NULL_CHECK(render_device, "Failed to get render device handle");

    i2s_render_cfg_t i2s_cfg = { .play_handle = render_device };
    renderer_system.audio_renderer = av_render_alloc_i2s_render(&i2s_cfg);
    NULL_CHECK(renderer_system.audio_renderer, "Failed to create I2S renderer");

    // Set initial speaker volume
    esp_codec_dev_set_out_vol(i2s_cfg.play_handle, CONFIG_LK_EXAMPLE_SPEAKER_VOLUME);
    ESP_LOGI(TAG, "Speaker volume set to %d", CONFIG_LK_EXAMPLE_SPEAKER_VOLUME);

    // Audio buffer + drop policy — tuned for jitter resilience over
    // absolute minimum latency.
    //
    // History:
    //   * 2*4096 raw / 16*1024 render / allow_drop_data=true was the
    //     original aggressive setting — saved ~85 ms but produced audible
    //     crackle under WiFi jitter (drops instead of buffer).
    //   * 4*4096 raw / 24*1024 render / drop=false eliminated most
    //     crackle but the user still heard a small amount.
    //   * The remaining crackle was traced to WiFi power-save naps
    //     (WIFI_PS_MIN_MODEM, ~307 ms listen interval) — packets arrive
    //     in bursts every ~300 ms. The render FIFO at 256 ms couldn't
    //     absorb the burst tail. Two-part fix: bump render FIFO to
    //     ~340 ms AND disable WiFi PS during voice sessions
    //     (see example.c::join_room).
    //
    // xAI Realtime's ~280 ms TTFA keeps total perceived latency under
    // 1 s even with the larger buffers.
    av_render_cfg_t render_cfg = {
        .audio_render = renderer_system.audio_renderer,
        .audio_raw_fifo_size = 4 * 4096,      // ~170 ms @ 48kHz mono — Opus FIFO
        .audio_render_fifo_size = 32 * 1024,  // ~340 ms PCM headroom for burst absorb
        .allow_drop_data = false,             // no silent drops; prefer latency over crackle
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

// Defer unmute by this many ms so the renderer's playback FIFO drains
// before the mic goes live again. Without this delay, the tail of the
// agent's utterance (up to ~256 ms still in audio_render_fifo_size at
// 24 KB / 48 kHz mono) leaks into the mic and gets sent back to the
// agent → it hears itself. Mute on "speaking" is still applied
// immediately so we never miss the start of the agent's audio.
#define MIC_UNMUTE_DRAIN_DELAY_MS 300
static TimerHandle_t s_unmute_timer = NULL;

static void apply_codec_mute(bool muted)
{
    esp_codec_dev_handle_t record_handle = get_record_handle();
    if (record_handle == NULL) {
        return;
    }
    int rc = esp_codec_dev_set_in_mute(record_handle, muted);
    if (rc != 0) {
        ESP_LOGW(TAG, "esp_codec_dev_set_in_mute(%d) failed: %d", (int)muted, rc);
    } else {
        ESP_LOGI(TAG, "Mic %s", muted ? "muted" : "unmuted");
    }
}

static void unmute_timer_cb(TimerHandle_t t)
{
    (void)t;
    apply_codec_mute(false);
}

void media_set_mic_muted(bool muted)
{
    // Hardware-level mute at the codec — when muted, the I2S input stream
    // delivers silence regardless of what's happening at the mic. We use
    // this for half-duplex echo suppression while the agent is speaking
    // (the agent.py side publishes data-channel "speaking"/"listening"
    // events; example.c::on_data_received drives this). Without AEC this
    // is what keeps the agent from hearing itself.
    //
    // Asymmetric timing: MUTE is applied immediately (don't want to miss
    // the start of the agent's utterance). UN-mute is DELAYED so the
    // ~256 ms of audio already buffered in the renderer FIFO finishes
    // playing through the speaker before we open the mic — otherwise the
    // tail of the agent's reply leaks back as fresh "user input."
    esp_codec_dev_handle_t record_handle = get_record_handle();
    if (record_handle == NULL) {
        return;
    }

    if (muted) {
        // Mute immediately. If an unmute was pending, cancel it.
        if (s_unmute_timer != NULL) {
            xTimerStop(s_unmute_timer, 0);
        }
        apply_codec_mute(true);
    } else {
        // Defer unmute by MIC_UNMUTE_DRAIN_DELAY_MS.
        if (s_unmute_timer == NULL) {
            s_unmute_timer = xTimerCreate(
                "mic_unmute",
                pdMS_TO_TICKS(MIC_UNMUTE_DRAIN_DELAY_MS),
                pdFALSE,  // one-shot
                NULL,
                unmute_timer_cb);
            if (s_unmute_timer == NULL) {
                // Timer alloc failed — fall back to immediate unmute. Rare;
                // would only happen if FreeRTOS timer queue is exhausted.
                ESP_LOGW(TAG, "unmute timer alloc failed; unmuting immediately");
                apply_codec_mute(false);
                return;
            }
        }
        xTimerStop(s_unmute_timer, 0);
        xTimerChangePeriod(s_unmute_timer, pdMS_TO_TICKS(MIC_UNMUTE_DRAIN_DELAY_MS), 0);
        xTimerStart(s_unmute_timer, 0);
    }
}

void media_cleanup(void)
{
    ESP_LOGI(TAG, "Cleaning up media systems...");

    // Stop and close capture system
    if (capturer_system.capturer_handle != NULL) {
        ESP_LOGI(TAG, "Closing capture system...");
        esp_capture_close(capturer_system.capturer_handle);
        capturer_system.capturer_handle = NULL;
    }

    // Stop and close renderer system
    if (renderer_system.av_renderer_handle != NULL) {
        ESP_LOGI(TAG, "Closing renderer system...");
        av_render_close(renderer_system.av_renderer_handle);
        renderer_system.av_renderer_handle = NULL;
    }

    ESP_LOGI(TAG, "Media cleanup complete");
}
