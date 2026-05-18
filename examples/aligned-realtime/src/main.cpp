#include "main.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>

#include "nvs_flash.h"

// C functions from media.c and livekit
extern "C" {
#include "media.h"
#include "livekit.h"
}

// Task handle for voice connection task
static TaskHandle_t voice_task_handle = NULL;

// Wheel button timing constants
#define SHORT_HOLD_TIME_MS  2000   // BSP fires long_press_cb at 2s
#define LONG_HOLD_TIME_MS   5000   // Power off at 5s total hold time
#define POWER_OFF_TIMER_MS  3000   // Additional 3s after long_press_cb for power off

// Wheel button state tracking
static int64_t g_long_press_start_time = 0;  // When long press callback was triggered
static TimerHandle_t g_power_off_timer = NULL;
static bool g_power_off_pending = false;
static bool g_disconnect_in_progress = false;  // Prevent double-disconnect

// board_init() is defined in board.c - no duplicate here

/**
 * Power off timer callback - fires 3s after long press detected (5s total hold)
 * This triggers full device shutdown
 */
static void power_off_timer_callback(TimerHandle_t xTimer)
{
  ESP_LOGI("WHEEL", "Power off timer expired - shutting down device");
  g_power_off_pending = true;

  // Show goodbye UI
  ui_powering_off();
  vTaskDelay(pdMS_TO_TICKS(500));

  // Mute audio and dim display
  bsp_codec_mute_set(true);
  bsp_lcd_brightness_set(0);

  // Shutdown the device
  ESP_LOGI("WHEEL", "Executing system shutdown...");
  bsp_system_shutdown();

  // If shutdown doesn't work (e.g., USB powered), restart instead
  vTaskDelay(pdMS_TO_TICKS(2000));
  ESP_LOGW("WHEEL", "Shutdown may not work on USB power, restarting...");
  esp_restart();
}

/**
 * Long press callback - fires at 2s hold time (BSP default)
 * Starts the power-off timer for additional 3s (5s total)
 */
extern "C" void long_press_event_cb(void)
{
  ESP_LOGI("WHEEL", "Long press detected (2s) - hold for 5s to power off, release to disconnect");

  // Record when long press was detected
  g_long_press_start_time = esp_timer_get_time();
  g_power_off_pending = false;

  // Create or reset the power-off timer
  if (g_power_off_timer == NULL) {
    g_power_off_timer = xTimerCreate(
      "power_off",
      pdMS_TO_TICKS(POWER_OFF_TIMER_MS),
      pdFALSE,  // One-shot timer
      NULL,
      power_off_timer_callback
    );
  }

  // Start the power-off timer
  if (g_power_off_timer != NULL) {
    xTimerReset(g_power_off_timer, 0);
    ESP_LOGI("WHEEL", "Power-off timer started (3s)");
  }
}

/**
 * Long press release callback - fires when button is released after long press
 * If released before 5s total, disconnect voice instead of power off
 */
extern "C" void long_release_event_cb(void)
{
  ESP_LOGI("WHEEL", "Long press released");

  // Cancel the power-off timer if it hasn't fired yet
  if (g_power_off_timer != NULL && !g_power_off_pending) {
    xTimerStop(g_power_off_timer, 0);
    ESP_LOGI("WHEEL", "Power-off timer cancelled");
  }

  // If power off is already pending, let it complete
  if (g_power_off_pending) {
    ESP_LOGI("WHEEL", "Power off already in progress");
    return;
  }

  // Prevent double-disconnect
  if (g_disconnect_in_progress) {
    ESP_LOGW("WHEEL", "Disconnect already in progress, ignoring");
    return;
  }

  // Short hold (2-5s) = disconnect voice session
  ESP_LOGI("WHEEL", "Short hold detected - disconnecting voice session");

  // Check if voice is active and disconnect
  if (livekit_is_active() || room_is_active()) {
    g_disconnect_in_progress = true;
    ui_disconnecting();

    // Do the disconnect
    ESP_LOGI("WHEEL", "Initiating clean disconnect...");
    livekit_stop();

    // Clear the voice task handle so user can reconnect
    voice_task_handle = NULL;
    g_disconnect_in_progress = false;

    ESP_LOGI("WHEEL", "Voice session disconnected - ready for reconnection");
  } else {
    ESP_LOGI("WHEEL", "No active voice session to disconnect");
    // If no voice session, just return to listening state
    ui_listening();
  }
}

// Voice connection task - runs aligned_start in background
static void voice_connection_task(void *pvParameters)
{
  ESP_LOGI("BUTTON", "Starting voice connection...");

  // Step 1: Get LiveKit credentials from Aligned backend
  esp_err_t err = aligned_get_livekit_credentials();
  if (err != ESP_OK) {
    ESP_LOGE("BUTTON", "Failed to connect to Aligned backend");
    ui_listening();  // Return to listening state
    voice_task_handle = NULL;  // Clear handle so user can reconnect
    vTaskDelete(NULL);
    return;
  }

  // Step 2: Start LiveKit voice connection using SDK signaling
  join_room();  // Non-blocking - room handles callbacks

  // Cleanup
  ESP_LOGI("BUTTON", "Voice connection ended, cleaning up task");
  ui_listening();
  voice_task_handle = NULL;  // Clear handle so user can reconnect
  vTaskDelete(NULL);
}

// Single click handler - start voice conversation
extern "C" void single_click_event_cb(void)
{
  ESP_LOGI("BUTTON", "Button pressed - starting voice conversation");

  // If disconnect is in progress, wait
  if (g_disconnect_in_progress) {
    ESP_LOGW("BUTTON", "Disconnect in progress, please wait");
    return;
  }

  // Don't start multiple voice tasks
  if (voice_task_handle != NULL) {
    ESP_LOGW("BUTTON", "Voice task already running");
    return;
  }

  // Don't start if already connected
  if (livekit_is_active() || room_is_active()) {
    ESP_LOGW("BUTTON", "Already connected to voice");
    return;
  }

  // Create voice connection task with large stack for WebRTC
  xTaskCreate(
    voice_connection_task,
    "voice_task",
    8192,  // 8KB stack
    NULL,
    5,     // Priority
    &voice_task_handle
  );
}

/**
 * Enter light sleep mode
 * Wakes up on button press
 */
static void enter_sleep_mode(void)
{
  ESP_LOGI("SLEEP", "Preparing to enter light sleep mode...");

  // If voice is active, disconnect first
  if (livekit_is_active() || room_is_active()) {
    ESP_LOGI("SLEEP", "Disconnecting voice before sleep...");
    livekit_stop();
    voice_task_handle = NULL;
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  // Show sleep UI
  ui_powering_off();  // Reuse power-off UI for now
  vTaskDelay(pdMS_TO_TICKS(500));

  // Mute audio and dim display
  bsp_codec_mute_set(true);
  bsp_lcd_brightness_set(0);

  ESP_LOGI("SLEEP", "Entering light sleep - press wheel button to wake");

  // Configure wakeup source - GPIO41 (KNOB_A) is used for encoder
  // but the button press goes through IO expander, so we use timer wakeup
  // for simplicity and check button state on wake
  esp_sleep_enable_timer_wakeup(60 * 1000000ULL);  // Wake every 60 seconds to check

  // Enter light sleep
  esp_light_sleep_start();

  // We woke up
  ESP_LOGI("SLEEP", "Woke up from sleep");

  // Restore display
  bsp_lcd_brightness_set(100);
  bsp_codec_mute_set(false);

  // Re-init UI
  ui_listening();

  ESP_LOGI("SLEEP", "Ready for voice connection");
}

// Sleep mode can be triggered via console command 'sleep'
// Double-click detection would require BSP modification

// Export sleep function for console command
extern "C" void device_enter_sleep(void)
{
  enter_sleep_mode();
}

extern "C" void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_event_loop_create_default());

  board_init();

  // Register button handlers for dual-timing wheel control
  // Short hold (2-5s) = disconnect voice, Long hold (5s+) = power off
  bsp_set_btn_long_press_cb(long_press_event_cb);      // Fires at 2s hold
  bsp_set_btn_long_release_cb(long_release_event_cb);  // Fires when released after long press
  bsp_set_btn_single_click_cb(single_click_event_cb);  // Single click = start voice conversation

  ui_init();
  oai_wifi_init();
  cmd_init();
  media_init();  // Initialize audio capturer and renderer for LiveKit SDK

  // Initialize LiveKit system (WebRTC, signaling, etc.)
  livekit_err_t lk_ret = livekit_system_init();
  if (lk_ret != LIVEKIT_ERR_NONE) {
    ESP_LOGE("MAIN", "Failed to initialize LiveKit system: %d", lk_ret);
  } else {
    ESP_LOGI("MAIN", "LiveKit system initialized successfully");
  }

  oai_wifi();
  ui_listening();

  // Aligned mode: Don't auto-connect via old OpenAI WebRTC
  // User will manually call aligned_connect to get LiveKit credentials
  // OR press the button to trigger voice connection
  // oai_webrtc(); // DISABLED for aligned-realtime

  ESP_LOGI("MAIN", "Ready! Press button to start voice conversation, or use 'aligned_start' command");
}
