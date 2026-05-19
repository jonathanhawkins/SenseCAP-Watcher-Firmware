#include <stdbool.h>
#include <stdio.h>
#include "esp_log.h"
#include "board.h"
#include "ui.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "example.h"
#include "livekit_example_utils.h"
#include "media.h"
#include "cmd.h"
#include "aligned_client.h"
#include "volume_control.h"
#include "wifi_scan.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "livekit.h"

static const char *TAG = "main";

#define BUTTON_POLL_MS        25
#define BUTTON_DEBOUNCE_MS    50
#define BUTTON_LONG_PRESS_MS  2000
#define BUTTON_SLEEP_MS       5000   // 5 sec hold = deep sleep (wake on button)
#define BUTTON_SHUTDOWN_MS    8000   // 8 sec hold = full shutdown

static void handle_single_click(void)
{
    if (!aligned_has_token())
    {
        ESP_LOGW(TAG, "No device token set. Use aligned_token -t watcher_xxx...");
        return;
    }

    if (room_is_active())
    {
        ESP_LOGI(TAG, "Room already active");
        return;
    }

    ESP_LOGI(TAG, "Button click - joining LiveKit room");
    join_room();
}

static void handle_long_release(void)
{
    if (!room_is_active())
    {
        ESP_LOGI(TAG, "No active room to leave");
        ui_listening();
        return;
    }

    ESP_LOGI(TAG, "Button hold - leaving LiveKit room");
    ui_disconnecting();
    leave_room();
    // Brief pause so user sees "Disconnecting..." before returning to idle screen
    vTaskDelay(pdMS_TO_TICKS(500));
    ui_show_wifi_button();
}

static void handle_deep_sleep(void)
{
    ESP_LOGI(TAG, "Button held for %dms - entering deep sleep", BUTTON_SLEEP_MS);
    ESP_LOGI(TAG, "Press button to wake up");

    // Clean up if connected
    if (room_is_active())
    {
        ESP_LOGI(TAG, "Disconnecting from room before sleep...");
        ui_disconnecting();
        leave_room();
    }

    ui_powering_off();
    vTaskDelay(pdMS_TO_TICKS(500));

    // Enter deep sleep - will wake on button press (IO expander interrupt)
    // Pass 0 for infinite sleep until button wake
    bsp_system_deep_sleep(0);
}

static void handle_shutdown(void)
{
    ESP_LOGI(TAG, "Button held for %dms - full shutdown", BUTTON_SHUTDOWN_MS);
    ESP_LOGI(TAG, "Press button or plug USB to power on");

    // Clean up if connected
    if (room_is_active())
    {
        ESP_LOGI(TAG, "Disconnecting from room before shutdown...");
        ui_disconnecting();
        leave_room();
    }

    ui_powering_off();
    vTaskDelay(pdMS_TO_TICKS(500));

    // Complete power off via IO expander
    bsp_system_shutdown();
}

static void button_task(void *arg)
{
    bool last_pressed = false;
    TickType_t last_change = 0;
    TickType_t press_start = 0;
    bool shutdown_triggered = false;
    // Per-press one-shot flags so the threshold-crossed UI callbacks fire
    // exactly once per hold (the loop polls every BUTTON_POLL_MS = 25 ms,
    // so without the flags we'd re-fire on every tick past the threshold).
    bool disconnect_ready_fired = false;
    bool sleep_ready_fired = false;

    for (;;)
    {
        bool pressed = board_is_knob_pressed();
        TickType_t now = xTaskGetTickCount();

        if (pressed != last_pressed)
        {
            uint32_t elapsed_ms = (now - last_change) * portTICK_PERIOD_MS;
            if (elapsed_ms >= BUTTON_DEBOUNCE_MS)
            {
                last_change = now;
                last_pressed = pressed;

                if (pressed)
                {
                    press_start = now;
                    shutdown_triggered = false;
                    disconnect_ready_fired = false;
                    sleep_ready_fired = false;
                    // Immediate visual feedback that the press is registered.
                    // No-op when not in a voice session.
                    ui_knob_hold_start();
                }
                else
                {
                    // Button released - check how long it was held
                    uint32_t held_ms = (now - press_start) * portTICK_PERIOD_MS;

                    // Revert the hint back to its steady text before
                    // dispatching. If a real disconnect runs, ui_disconnecting()
                    // will overwrite this immediately.
                    ui_knob_hold_end();

                    // Skip release handling if shutdown was triggered
                    if (!shutdown_triggered)
                    {
                        if (held_ms >= BUTTON_SLEEP_MS)
                        {
                            // 5+ sec hold and release = deep sleep
                            handle_deep_sleep();
                        }
                        else if (held_ms >= BUTTON_LONG_PRESS_MS)
                        {
                            // 2+ sec hold and release = leave room
                            handle_long_release();
                        }
                        else
                        {
                            // Quick press = join room
                            handle_single_click();
                        }
                    }
                }
            }
        }

        // While held: fire threshold-crossed UI updates so the user knows
        // what releasing now will do (the actual dispatch still runs on
        // release, in the branch above).
        if (last_pressed && !shutdown_triggered)
        {
            uint32_t held_ms = (now - press_start) * portTICK_PERIOD_MS;

            if (!disconnect_ready_fired && held_ms >= BUTTON_LONG_PRESS_MS)
            {
                disconnect_ready_fired = true;
                ui_knob_hold_ready_disconnect();
            }
            if (!sleep_ready_fired && held_ms >= BUTTON_SLEEP_MS)
            {
                sleep_ready_fired = true;
                ui_knob_hold_ready_sleep();
            }

            // 8+ seconds = immediate shutdown (emergency power off)
            if (held_ms >= BUTTON_SHUTDOWN_MS)
            {
                shutdown_triggered = true;
                handle_shutdown();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

/**
 * @brief Connect to WiFi at boot.
 *
 * Strategy:
 *   1. Init the wifi_scan module (which also kicks an auto-connect to the
 *      IDF builtin slot — the previous most-recent network).
 *   2. Wait briefly (8s) for that to succeed. If the most-recent network is
 *      in range, we connect fast without burning a scan cycle.
 *   3. Otherwise, scan-and-pick-best-saved: walk the 8-slot saved list in
 *      LRU order and connect to the first one that's visible in the scan.
 *   4. Start a background task that keeps trying every 30s while disconnected
 *      (handles "WiFi briefly dropped, then came back" plus first-boot
 *      recovery if step 3 found nothing initially).
 */
static bool connect_wifi_from_flash(void)
{
    ESP_LOGI(TAG, "=== WiFi Connection Start ===");

    // Initialize the WiFi scan/connection module
    wifi_scan_init();

    // Always start the auto-reconnect watchdog. It no-ops while connected
    // and quietly recovers in the background if WiFi drops mid-session.
    wifi_start_auto_reconnect_task(30000);

    bool any_saved = wifi_has_saved_credentials();
    {
        char first_saved[1][WIFI_SSID_MAX_LEN];
        if (wifi_list_saved_ssids(first_saved, 1) > 0) {
            any_saved = true;
        }
    }

    if (!any_saved)
    {
        ESP_LOGW(TAG, "No WiFi credentials saved in flash");
        ESP_LOGI(TAG, "Use: wifi_sta -s \"SSID\" -p \"password\" then reboot");
        return false;
    }

    // Phase 1: fast-path — give the IDF builtin slot ~8s to succeed.
    // If the most-recently-used network is in range, this avoids a scan.
    ESP_LOGI(TAG, "Trying IDF builtin slot (fast-path, timeout: 8s)...");

    for (int i = 0; i < 80; i++)  // 8 second fast-path (80 * 100ms)
    {
        wifi_connection_state_t state = wifi_get_state();

        if (state == WIFI_STATE_CONNECTED)
        {
            ESP_LOGI(TAG, "✅ WiFi connected (fast-path)!");
            ESP_LOGI(TAG, "   SSID: %s", wifi_get_current_ssid());
            ESP_LOGI(TAG, "   IP:   %s", wifi_get_current_ip());
            ESP_LOGI(TAG, "=== WiFi Connection Complete ===");
            return true;
        }

        // FAILED here means the builtin-slot SSID isn't in range or has a
        // bad password — fall through to the scan-based path rather than
        // giving up.
        if (state == WIFI_STATE_FAILED)
        {
            ESP_LOGI(TAG, "Builtin-slot AP unreachable — falling back to scan");
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Phase 2: scan-and-pick-best — for when the LRU network is out of
    // range. Walks all saved networks and picks the first visible one.
    ESP_LOGI(TAG, "Scanning for any reachable saved network...");
    if (wifi_try_connect_best_saved(6000, 15000))
    {
        ESP_LOGI(TAG, "✅ WiFi connected (scan-and-pick)!");
        ESP_LOGI(TAG, "   SSID: %s", wifi_get_current_ssid());
        ESP_LOGI(TAG, "   IP:   %s", wifi_get_current_ip());
        ESP_LOGI(TAG, "=== WiFi Connection Complete ===");
        return true;
    }

    ESP_LOGW(TAG, "❌ No saved WiFi network reachable at boot");
    ESP_LOGI(TAG, "Auto-reconnect task will keep trying every 30s in the background");
    return false;
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    // Silence per-second AEC mic-level spam — it's once a second forever and
    // drowns out actual debug logs in the UART buffer.
    esp_log_level_set("AUD_AEC_SRC", ESP_LOG_WARN);

    // CRITICAL: Initialize NVS flash before anything else
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // CRITICAL: Create default event loop before any WiFi/network operations
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    livekit_system_init();
    board_init();
    media_init();

    // Initialize volume control with rotary encoder wheel support
    // Pass NULL since we use GPIO polling, not LVGL encoder input device
    volume_control_init(NULL);

    xTaskCreate(button_task, "knob_btn", 8192, NULL, 4, NULL);  // Increased from 3072 - HTTP requests need more stack

    // Initialize SNTP for time synchronization
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_config);

    // Connect to WiFi BEFORE starting REPL (so logs are visible)
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Starting WiFi connection...");
    ESP_LOGI(TAG, "========================================");
    fflush(stdout);

    // Try to connect to WiFi using credentials saved to flash
    // If no credentials, user can configure via console: wifi_sta -s "SSID" -p "password"
    if (connect_wifi_from_flash())
    {
        ESP_LOGI(TAG, "✅ WiFi connected!");
        ESP_LOGI(TAG, "");

        // If device token is configured, prompt for button/command to join
        if (aligned_has_token())
        {
            ESP_LOGI(TAG, "Device token found. Press the knob button or run aligned_start to join.");
        }
        else
        {
            ESP_LOGW(TAG, "⚠️  Device token not configured!");
            ESP_LOGI(TAG, "Set token with: aligned_token -t watcher_xxx...");
        }
    }
    else
    {
        ESP_LOGW(TAG, "❌ WiFi not connected");
        ESP_LOGI(TAG, "Configure WiFi with:");
        ESP_LOGI(TAG, "  wifi_sta -s \"YourSSID\" -p \"YourPassword\"");
        ESP_LOGI(TAG, "  reboot");
    }

    // Show WiFi setup button at bottom of screen (auto-hides when connected)
    ui_show_wifi_button();

    // Now start the console REPL (this will capture stdout)
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Starting console REPL...");
    fflush(stdout);
    cmd_init();

    // Console REPL continues running in background
}
