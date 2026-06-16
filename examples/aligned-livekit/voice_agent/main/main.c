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
#include "esp_sleep.h"

#include "livekit.h"

static const char *TAG = "main";

#define BUTTON_POLL_MS          25
#define BUTTON_DEBOUNCE_MS      50
#define BUTTON_CONNECT_MS       1000   // 1 sec hold = connect/join room (idle home only). A deliberate hold guards against a brush of the knob starting a voice session by accident.
#define BUTTON_CONNECT_DWELL_MS 600    // how long the green "Release to connect" state stays before the hold UX transitions into the sleep countdown
#define BUTTON_LONG_PRESS_MS    1750   // 1.75 sec hold = disconnect (in-room only; was 2250, snapped tighter per user feedback)
#define BUTTON_SLEEP_MS         5000   // 5 sec hold = deep sleep (wake on button)
#define BUTTON_SHUTDOWN_MS      8000   // 8 sec hold = full shutdown

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

    // A held knob during a live meeting ends the meeting (same as the End
    // button) so the server-side transcript is finalized cleanly.
    if (meeting_is_active())
    {
        ESP_LOGI(TAG, "Button hold - ending live meeting");
        stop_meeting();
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
    // Context for the in-progress hold, latched at press time. Both the hold
    // UX and the release action key off this rather than re-reading
    // room_is_active() every tick — otherwise a room dropping mid-hold
    // (CONNECTED -> DISCONNECTED) would flip the UI from the disconnect flow
    // to the connect flow on the next poll. Latching freezes the gesture's
    // meaning for its whole duration.
    bool hold_in_room = false;
    // Per-press one-shot flags so the threshold-crossed UI callbacks fire
    // exactly once per hold (the loop polls every BUTTON_POLL_MS = 25 ms,
    // so without the flags we'd re-fire on every tick past the threshold).
    bool connect_ready_fired = false;    // idle: crossed BUTTON_CONNECT_MS (green "Release to connect")
    bool sleep_phase_entered = false;    // idle: hold UX handed off to the sleep countdown
    bool disconnect_ready_fired = false; // in-room: crossed BUTTON_LONG_PRESS_MS
    bool sleep_ready_fired = false;      // both: crossed BUTTON_SLEEP_MS

    for (;;)
    {
        // Service deferred Live Meeting / End button taps off the LVGL task.
        service_meeting_requests();

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
                    connect_ready_fired = false;
                    sleep_phase_entered = false;
                    disconnect_ready_fired = false;
                    sleep_ready_fired = false;
                    // Freeze the gesture's context for the whole hold (see the
                    // hold_in_room comment above).
                    hold_in_room = room_is_active();
                    // Immediate visual feedback that the press is registered:
                    // idle home -> "Hold to connect...", in a room -> "Hold to
                    // disconnect...".
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

                    // Skip release handling if shutdown already fired mid-hold.
                    if (!shutdown_triggered)
                    {
                        if (held_ms >= BUTTON_SLEEP_MS)
                        {
                            // 5 s+ -> deep sleep (same gesture in both contexts).
                            handle_deep_sleep();
                        }
                        else if (ui_plan_is_active() && held_ms < BUTTON_LONG_PRESS_MS)
                        {
                            // Plan-of-day picker is showing: a short press confirms
                            // the knob-highlighted block (the agent books it). A
                            // longer hold still falls through to disconnect/sleep
                            // below as an escape hatch.
                            plan_confirm_selection();
                        }
                        else if (hold_in_room)
                        {
                            // In a voice room: 1.75 s+ leaves the room. A briefer
                            // tap is ignored so a stray bump never hangs up a
                            // live session.
                            if (held_ms >= BUTTON_LONG_PRESS_MS)
                            {
                                handle_long_release();
                            }
                        }
                        else
                        {
                            // Idle home: 1 s+ joins the room. A tap shorter than
                            // BUTTON_CONNECT_MS is ignored so a brush of the knob
                            // never starts a voice session by accident.
                            if (held_ms >= BUTTON_CONNECT_MS)
                            {
                                handle_single_click();
                            }
                        }
                    }
                }
            }
        }

        // While held: drive the progress bar + fire the threshold-crossed
        // hint updates so the user can see what releasing now will do. The
        // actual dispatch still happens on release (branch above). Context is
        // the latched hold_in_room, never a fresh room_is_active(), so a
        // mid-hold room drop can't swap the disconnect flow for the connect
        // flow.
        if (last_pressed && !shutdown_triggered)
        {
            uint32_t held_ms = (now - press_start) * portTICK_PERIOD_MS;

            if (hold_in_room)
            {
                // ===== In a voice room: disconnect (1.75 s) -> sleep (5 s) =====
                // White fill toward the disconnect threshold; ready_disconnect
                // pins it green at 1.75 s, ready_sleep swaps it amber at 5 s.
                // Once either ready-state has fired we stop pushing the white
                // fill so it can't overwrite the pinned colour.
                bool progress_pinned = disconnect_ready_fired || sleep_ready_fired;
                if (!progress_pinned)
                {
                    uint32_t pct = (held_ms * 100U) / BUTTON_LONG_PRESS_MS;
                    if (pct > 100) pct = 100;
                    ui_knob_hold_progress((uint8_t)pct);
                }
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
            }
            else
            {
                // ===== Idle home: connect (1 s) -> [dwell] -> sleep (5 s) =====
                // Two-phase fill so the connect gesture and the sleep gesture
                // each get the full bar and an unmistakable hand-off between
                // them:
                //   phase 1  [0, 1 s)              white fill toward connect
                //   ready    at 1 s                green "Release to connect"
                //   dwell    [1 s, 1 s+600 ms)     green held so it's legible
                //   phase 2  [1 s+600 ms, 5 s)     amber refill toward sleep
                //   ready    at 5 s                amber "Release for sleep"
                // A release anywhere in [1 s, 5 s) connects; the amber phase-2
                // copy only signals what holding LONGER will do.
                const uint32_t sleep_fill_start = BUTTON_CONNECT_MS + BUTTON_CONNECT_DWELL_MS;
                if (held_ms < BUTTON_CONNECT_MS)
                {
                    uint32_t pct = (held_ms * 100U) / BUTTON_CONNECT_MS;
                    if (pct > 100) pct = 100;
                    ui_knob_hold_progress((uint8_t)pct);
                }
                else if (!connect_ready_fired)
                {
                    connect_ready_fired = true;
                    ui_knob_hold_ready_connect();
                }
                else if (held_ms >= sleep_fill_start && !sleep_ready_fired)
                {
                    // First tick of phase 2: hand the hold UX off from connect
                    // to the sleep countdown (resets the bar to empty amber),
                    // then refill it toward the 5 s sleep threshold.
                    if (!sleep_phase_entered)
                    {
                        sleep_phase_entered = true;
                        ui_knob_hold_enter_sleep_phase();
                    }
                    uint32_t span = BUTTON_SLEEP_MS - sleep_fill_start;
                    uint32_t pct = ((held_ms - sleep_fill_start) * 100U) / span;
                    if (pct > 100) pct = 100;
                    ui_knob_hold_progress((uint8_t)pct);
                }
                if (!sleep_ready_fired && held_ms >= BUTTON_SLEEP_MS)
                {
                    sleep_ready_fired = true;
                    ui_knob_hold_ready_sleep();
                }
            }

            // 8 s+ -> immediate emergency power off (fires while still held).
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

// ---- Background device-token auto-refresh -------------------------------
// The device token expires 90 days after issue (DEFAULT_TOKEN_EXPIRY_DAYS on
// the backend). This task periodically POSTs /refresh-token so it never lapses
// during active use. Notes:
//   * Skips while a voice room is active — a WiFi burst during a call can
//     starve the audio render thread (see watcher-playback-crackle), so we
//     defer to a short retry instead.
//   * The backend's refresh RPC refuses an ALREADY-expired token, so a device
//     left offline past expiry still needs a one-time re-provision; the failed
//     refresh just logs and retries.
//   * vTaskDelay is done in 1-minute chunks: pdMS_TO_TICKS(12h) overflows
//     TickType_t at the 100 Hz tick rate (43.2M ms * 100 > 2^32).
#define TOKEN_REFRESH_INITIAL_DELAY_MIN 2     // let WiFi/SNTP settle after boot
#define TOKEN_REFRESH_INTERVAL_MIN      720   // 12 h — keeps expiry ~90 days out
#define TOKEN_REFRESH_BUSY_RETRY_MIN    5     // recheck soon if a call was active

static void delay_minutes(int minutes)
{
    for (int i = 0; i < minutes; i++) {
        vTaskDelay(pdMS_TO_TICKS(60 * 1000));
    }
}

static void token_refresh_task(void *arg)
{
    delay_minutes(TOKEN_REFRESH_INITIAL_DELAY_MIN);

    for (;;) {
        int wait_min = TOKEN_REFRESH_INTERVAL_MIN;

        if (!aligned_has_token()) {
            // Nothing to refresh yet (e.g. unprovisioned device); check later.
        } else if (room_is_active()) {
            ESP_LOGI(TAG, "Token auto-refresh deferred: voice room active");
            wait_min = TOKEN_REFRESH_BUSY_RETRY_MIN;
        } else if (aligned_refresh_token() != ESP_OK) {
            ESP_LOGW(TAG, "Token auto-refresh failed (will retry next cycle)");
        }

        delay_minutes(wait_min);
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    // Silence per-second AEC mic-level spam — it's once a second forever and
    // drowns out actual debug logs in the UART buffer.
    esp_log_level_set("AUD_AEC_SRC", ESP_LOG_WARN);

    // Log both reset_reason (why the CPU booted) and wake_cause (which
    // peripheral, if any, woke deep sleep). Together they distinguish:
    //   - reset=POWERON, wake=UNDEFINED  → cold boot / USB plug
    //   - reset=DEEPSLEEP, wake=EXT0     → wake from deep sleep. Could be
    //     a real knob press OR a spurious charge-controller wake; the
    //     board_init spurious-wake recovery re-sleeps if the knob isn't
    //     actually pressed. See .claude/rules/watcher-power.md.
    //   - reset=BROWNOUT / PANIC / WDT   → not a deep-sleep wake at all;
    //     peripheral power-down dropped a rail or something panicked
    esp_reset_reason_t reset_reason = esp_reset_reason();
    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    const char *reset_str = "?";
    switch (reset_reason) {
        case ESP_RST_POWERON:   reset_str = "POWERON";   break;
        case ESP_RST_EXT:       reset_str = "EXT";       break;
        case ESP_RST_SW:        reset_str = "SW";        break;
        case ESP_RST_PANIC:     reset_str = "PANIC";     break;
        case ESP_RST_INT_WDT:   reset_str = "INT_WDT";   break;
        case ESP_RST_TASK_WDT:  reset_str = "TASK_WDT";  break;
        case ESP_RST_WDT:       reset_str = "WDT";       break;
        case ESP_RST_DEEPSLEEP: reset_str = "DEEPSLEEP"; break;
        case ESP_RST_BROWNOUT:  reset_str = "BROWNOUT";  break;
        case ESP_RST_SDIO:      reset_str = "SDIO";      break;
        default: break;
    }
    const char *wake_str = "?";
    switch (wake_cause) {
        case ESP_SLEEP_WAKEUP_UNDEFINED: wake_str = "UNDEFINED"; break;
        case ESP_SLEEP_WAKEUP_EXT0:      wake_str = "EXT0";      break;
        case ESP_SLEEP_WAKEUP_EXT1:      wake_str = "EXT1";      break;
        case ESP_SLEEP_WAKEUP_TIMER:     wake_str = "TIMER";     break;
        case ESP_SLEEP_WAKEUP_TOUCHPAD:  wake_str = "TOUCHPAD";  break;
        case ESP_SLEEP_WAKEUP_ULP:       wake_str = "ULP";       break;
        case ESP_SLEEP_WAKEUP_GPIO:      wake_str = "GPIO";      break;
        default: break;
    }
    ESP_LOGI(TAG, "Boot: reset_reason=%s wake_cause=%s", reset_str, wake_str);

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

        // Keep the 90-day device token alive while the device is in use.
        // No-ops until a token is configured, so it's safe to start here even
        // on an unprovisioned device. HTTPS handshake needs the larger stack.
        xTaskCreate(token_refresh_task, "tok_refresh", 8192, NULL, 3, NULL);
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
