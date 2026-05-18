#include <driver/i2s.h>
#include <opus.h>
#include <esp_log.h>

#include "main.h"

static const char *TAG = "media";

#define OPUS_OUT_BUFFER_SIZE 1276  // 1276 bytes is recommended by opus_encode
#define SAMPLE_RATE  16000
#define CHANNELS     1

#define BUFFER_SAMPLES (640)
#define BUFFER_SAMPLES_CNT (BUFFER_SAMPLES/2)

#define OPUS_ENCODER_BITRATE 30000
#define OPUS_ENCODER_COMPLEXITY 0

static esp_codec_dev_handle_t play_dev_handle;
static esp_codec_dev_handle_t record_dev_handle;

void oai_init_audio_capture() {
  bsp_codec_mute_set(true);
  bsp_codec_mute_set(false);
  bsp_codec_volume_set(100, NULL);

  play_dev_handle = bsp_codec_speaker_get();
  record_dev_handle = bsp_codec_microphone_get();
  return;
}

opus_int16 *output_buffer = NULL;
OpusDecoder *opus_decoder = NULL;

bool oai_init_audio_decoder() {
  int decoder_error = 0;
  opus_decoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &decoder_error);
  if (decoder_error != OPUS_OK) {
    ESP_LOGE(TAG, "Failed to create OPUS decoder, error: %d", decoder_error);
    return false;
  }
  output_buffer = (opus_int16 *)malloc(BUFFER_SAMPLES_CNT * sizeof(opus_int16));
  if (output_buffer == NULL) {
    ESP_LOGE(TAG, "Failed to allocate decoder output buffer (%d bytes)",
             (int)(BUFFER_SAMPLES_CNT * sizeof(opus_int16)));
    return false;
  }
  ESP_LOGI(TAG, "Audio decoder initialized successfully");
  return true;
}

void oai_audio_decode(uint8_t *data, size_t size) {
  if (opus_decoder == NULL || output_buffer == NULL) {
    ESP_LOGE(TAG, "Decoder not initialized!");
    return;
  }

  int decoded_size =
      opus_decode(opus_decoder, data, size, output_buffer, BUFFER_SAMPLES_CNT, 0);

  if (size > 26) {
    ESP_LOGD(TAG, "decode: in=%d out=%d", (int)size, decoded_size);
    ui_switch_speaking();
  }
  if (decoded_size > 0) {
    esp_codec_dev_write(play_dev_handle, output_buffer, BUFFER_SAMPLES_CNT * sizeof(opus_int16));
  }
}

OpusEncoder *opus_encoder = NULL;
opus_int16 *encoder_input_buffer = NULL;
uint8_t *encoder_output_buffer = NULL;

bool oai_init_audio_encoder() {
  int encoder_error;
  opus_encoder = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_VOIP,
                                     &encoder_error);
  if (encoder_error != OPUS_OK) {
    ESP_LOGE(TAG, "Failed to create OPUS encoder, error: %d", encoder_error);
    return false;
  }

  if (opus_encoder_init(opus_encoder, SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_VOIP) !=
      OPUS_OK) {
    ESP_LOGE(TAG, "Failed to initialize OPUS encoder");
    return false;
  }

  opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE(OPUS_ENCODER_BITRATE));
  opus_encoder_ctl(opus_encoder, OPUS_SET_COMPLEXITY(OPUS_ENCODER_COMPLEXITY));
  opus_encoder_ctl(opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

  encoder_input_buffer = (opus_int16 *)malloc(BUFFER_SAMPLES);
  if (encoder_input_buffer == NULL) {
    ESP_LOGE(TAG, "Failed to allocate encoder input buffer (%d bytes)", BUFFER_SAMPLES);
    return false;
  }

  encoder_output_buffer = (uint8_t *)malloc(OPUS_OUT_BUFFER_SIZE);
  if (encoder_output_buffer == NULL) {
    ESP_LOGE(TAG, "Failed to allocate encoder output buffer (%d bytes)", OPUS_OUT_BUFFER_SIZE);
    free(encoder_input_buffer);
    encoder_input_buffer = NULL;
    return false;
  }

  ESP_LOGI(TAG, "Audio encoder initialized successfully");
  return true;
}

bool oai_send_audio(PeerConnection *peer_connection) {
  if (opus_encoder == NULL || encoder_input_buffer == NULL || encoder_output_buffer == NULL) {
    ESP_LOGE(TAG, "Encoder not initialized!");
    return false;
  }

  esp_codec_dev_read(record_dev_handle, encoder_input_buffer, BUFFER_SAMPLES);

  auto encoded_size =
      opus_encode(opus_encoder, encoder_input_buffer, BUFFER_SAMPLES_CNT,
                  encoder_output_buffer, OPUS_OUT_BUFFER_SIZE);

  if (encoded_size > 0) {
    peer_connection_send_audio(peer_connection, encoder_output_buffer, encoded_size);
  }
  return true;
}
