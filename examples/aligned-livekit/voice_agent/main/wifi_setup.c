/**
 * @file wifi_setup.c
 * @brief WiFi status and setup UI screen implementation
 *
 * Displays WiFi connection status and provides navigation to network setup.
 * Uses the Page Manager approach for screen transitions, matching the
 * volume_control.c pattern.
 */

#include "wifi_setup.h"
#include "wifi_scan.h"
#include "wifi_list.h"
#include "pm.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "wifi_setup";

// Screen dimensions (412x412 round display)
#define SCREEN_WIDTH  412
#define SCREEN_HEIGHT 412

// Colors matching the device theme
#define COLOR_BACKGROUND    0x1a1a1a   // Dark background
#define COLOR_CARD_BG       0x252525   // Card/container background
#define COLOR_TEXT_PRIMARY  0xFFFFFF   // White text
#define COLOR_TEXT_SECONDARY 0x888888  // Gray text for secondary info
#define COLOR_ACCENT        0x8FC31F   // Green accent (matches volume control)
#define COLOR_CONNECTED     0x4CAF50   // Green for connected status
#define COLOR_DISCONNECTED  0xFF5722   // Orange/red for disconnected
#define COLOR_BUTTON_BG     0x333333   // Button background
#define COLOR_BUTTON_PRESSED 0x444444  // Button pressed state

// WiFi setup screen objects
static lv_obj_t *ui_Page_WiFi = NULL;           // Main WiFi screen
static lv_obj_t *ui_wifi_title = NULL;          // "WiFi Setup" title
static lv_obj_t *ui_wifi_icon = NULL;           // WiFi status icon
static lv_obj_t *ui_status_label = NULL;        // "Connected" / "Not Connected"
static lv_obj_t *ui_ssid_label = NULL;          // Network name
static lv_obj_t *ui_ip_label = NULL;            // IP address
static lv_obj_t *ui_signal_label = NULL;        // Signal strength
static lv_obj_t *ui_action_btn = NULL;          // "Scan Networks" or "Change Network"
static lv_obj_t *ui_action_btn_label = NULL;    // Button text
static lv_obj_t *ui_back_btn = NULL;            // Back button
static lv_obj_t *ui_back_btn_label = NULL;      // Back button text
static lv_obj_t *ui_info_container = NULL;      // Container for connection info

// Group info for page manager
static GroupInfo group_page_wifi;

// Connection monitoring timer
static lv_timer_t *s_status_refresh_timer = NULL;
#define STATUS_REFRESH_INTERVAL_MS 500

// Spinner animation frame counter
static uint8_t s_spinner_frame = 0;
static const char *SPINNER_FRAMES[] = { "◐", "◓", "◑", "◒" };
#define SPINNER_FRAME_COUNT 4

// Forward declarations
static void ui_Page_WiFi_screen_init(void);
static void update_wifi_status_display(void);
static void action_btn_event_cb(lv_event_t *e);
static void back_btn_event_cb(lv_event_t *e);
static void status_refresh_timer_cb(lv_timer_t *timer);
static void start_status_timer(void);
static void stop_status_timer(void);

/**
 * @brief Get signal strength description from RSSI value
 */
static const char* get_signal_description(int8_t rssi)
{
    if (rssi >= -50) return "Excellent";
    if (rssi >= -60) return "Good";
    if (rssi >= -70) return "Fair";
    if (rssi >= -80) return "Weak";
    return "Very Weak";
}

/**
 * @brief Get signal icon/color based on RSSI
 */
static lv_color_t get_signal_color(int8_t rssi)
{
    if (rssi >= -60) return lv_color_hex(COLOR_CONNECTED);      // Green
    if (rssi >= -75) return lv_color_hex(0xFFC107);             // Yellow/amber
    return lv_color_hex(COLOR_DISCONNECTED);                     // Red
}

/**
 * @brief Create the WiFi setup screen UI
 */
static void ui_Page_WiFi_screen_init(void)
{
    if (ui_Page_WiFi != NULL) {
        return;  // Already created
    }

    ESP_LOGI(TAG, "Creating WiFi setup screen");

    // Create screen with dark background
    ui_Page_WiFi = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_Page_WiFi, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Page_WiFi, lv_color_hex(COLOR_BACKGROUND), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_Page_WiFi, 255, LV_PART_MAIN);

    // Title: "WiFi Setup"
    ui_wifi_title = lv_label_create(ui_Page_WiFi);
    lv_label_set_text(ui_wifi_title, "WiFi Setup");
    lv_obj_set_style_text_color(ui_wifi_title, lv_color_hex(COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_wifi_title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(ui_wifi_title, LV_ALIGN_TOP_MID, 0, 40);

    // WiFi icon (using LVGL symbol) - centered and large
    ui_wifi_icon = lv_label_create(ui_Page_WiFi);
    lv_label_set_text(ui_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(ui_wifi_icon, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_wifi_icon, lv_color_hex(COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_align(ui_wifi_icon, LV_ALIGN_CENTER, 0, -80);

    // Status label: "Connected" / "Not Connected"
    ui_status_label = lv_label_create(ui_Page_WiFi);
    lv_label_set_text(ui_status_label, "Checking...");
    lv_obj_set_style_text_color(ui_status_label, lv_color_hex(COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_status_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(ui_status_label, LV_ALIGN_CENTER, 0, -30);

    // Info container for connection details
    ui_info_container = lv_obj_create(ui_Page_WiFi);
    lv_obj_set_size(ui_info_container, 280, 100);
    lv_obj_align(ui_info_container, LV_ALIGN_CENTER, 0, 30);
    lv_obj_clear_flag(ui_info_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_info_container, lv_color_hex(COLOR_CARD_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_info_container, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(ui_info_container, 16, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_info_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui_info_container, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(ui_info_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_info_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // SSID label (network name)
    ui_ssid_label = lv_label_create(ui_info_container);
    lv_label_set_text(ui_ssid_label, "");
    lv_obj_set_style_text_color(ui_ssid_label, lv_color_hex(COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_ssid_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(ui_ssid_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // IP address label
    ui_ip_label = lv_label_create(ui_info_container);
    lv_label_set_text(ui_ip_label, "");
    lv_obj_set_style_text_color(ui_ip_label, lv_color_hex(COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_ip_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(ui_ip_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // Signal strength label
    ui_signal_label = lv_label_create(ui_info_container);
    lv_label_set_text(ui_signal_label, "");
    lv_obj_set_style_text_color(ui_signal_label, lv_color_hex(COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_signal_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(ui_signal_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // Button container at bottom - side by side layout for round display
    // Position buttons side by side at the bottom to avoid overlap

    // Back button (left side)
    ui_back_btn = lv_btn_create(ui_Page_WiFi);
    lv_obj_set_size(ui_back_btn, 100, 40);
    lv_obj_align(ui_back_btn, LV_ALIGN_BOTTOM_MID, -70, -45);  // Left of center
    lv_obj_set_style_bg_color(ui_back_btn, lv_color_hex(COLOR_BUTTON_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_back_btn, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(ui_back_btn, 20, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ui_back_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_back_btn, lv_color_hex(COLOR_BUTTON_PRESSED), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(ui_back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);

    ui_back_btn_label = lv_label_create(ui_back_btn);
    lv_label_set_text(ui_back_btn_label, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(ui_back_btn_label, lv_color_hex(COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_back_btn_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(ui_back_btn_label);

    // Action button: "Scan Networks" or "Change Network" (right side)
    ui_action_btn = lv_btn_create(ui_Page_WiFi);
    lv_obj_set_size(ui_action_btn, 130, 40);
    lv_obj_align(ui_action_btn, LV_ALIGN_BOTTOM_MID, 55, -45);  // Right of center
    lv_obj_set_style_bg_color(ui_action_btn, lv_color_hex(COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_action_btn, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(ui_action_btn, 20, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ui_action_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_action_btn, lv_color_hex(0x7AAE1A), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(ui_action_btn, action_btn_event_cb, LV_EVENT_CLICKED, NULL);

    ui_action_btn_label = lv_label_create(ui_action_btn);
    lv_label_set_text(ui_action_btn_label, "Scan");
    lv_obj_set_style_text_color(ui_action_btn_label, lv_color_hex(0x000000), LV_PART_MAIN);  // Black text on green
    lv_obj_set_style_text_font(ui_action_btn_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(ui_action_btn_label);

    // Setup group info for encoder navigation
    // Add focusable elements: action button, back button
    group_page_wifi.obj_count = 2;
    group_page_wifi.group[0] = ui_action_btn;
    group_page_wifi.group[1] = ui_back_btn;

    // Add focus styling for encoder navigation
    lv_obj_set_style_outline_color(ui_action_btn, lv_color_hex(COLOR_ACCENT), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(ui_action_btn, 3, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_opa(ui_action_btn, 255, LV_PART_MAIN | LV_STATE_FOCUSED);

    lv_obj_set_style_outline_color(ui_back_btn, lv_color_hex(COLOR_ACCENT), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(ui_back_btn, 3, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_opa(ui_back_btn, 255, LV_PART_MAIN | LV_STATE_FOCUSED);

    ESP_LOGI(TAG, "WiFi setup screen created: %p", ui_Page_WiFi);
}

/**
 * @brief Update the WiFi status display with current connection info
 */
static void update_wifi_status_display(void)
{
    if (ui_Page_WiFi == NULL) {
        return;
    }

    bool is_connected = wifi_is_connected();

    if (is_connected) {
        // Update icon color to green
        lv_obj_set_style_text_color(ui_wifi_icon, lv_color_hex(COLOR_CONNECTED), LV_PART_MAIN);

        // Update status label
        lv_label_set_text(ui_status_label, "Connected");
        lv_obj_set_style_text_color(ui_status_label, lv_color_hex(COLOR_CONNECTED), LV_PART_MAIN);

        // Get connection details
        const char *ssid = wifi_get_current_ssid();
        const char *ip = wifi_get_current_ip();

        // Update SSID
        if (ssid && ssid[0] != '\0') {
            lv_label_set_text(ui_ssid_label, ssid);
        } else {
            lv_label_set_text(ui_ssid_label, "Unknown Network");
        }

        // Update IP address
        if (ip && ip[0] != '\0') {
            char ip_buf[32];
            snprintf(ip_buf, sizeof(ip_buf), "IP: %s", ip);
            lv_label_set_text(ui_ip_label, ip_buf);
        } else {
            lv_label_set_text(ui_ip_label, "IP: Obtaining...");
        }

        // Get signal strength from scan results
        // Find the connected network in scan results to get RSSI
        int8_t rssi = -100;  // Default to very weak
        uint16_t count = wifi_scan_get_count();
        const wifi_scan_result_t *results = wifi_scan_get_results();
        if (results != NULL && ssid != NULL) {
            for (uint16_t i = 0; i < count; i++) {
                if (results[i].is_connected) {
                    rssi = results[i].rssi;
                    break;
                }
            }
        }

        // Update signal info
        char signal_buf[48];
        snprintf(signal_buf, sizeof(signal_buf), "Signal: %d dBm (%s)", rssi, get_signal_description(rssi));
        lv_label_set_text(ui_signal_label, signal_buf);
        lv_obj_set_style_text_color(ui_signal_label, get_signal_color(rssi), LV_PART_MAIN);

        // Show info container
        lv_obj_clear_flag(ui_info_container, LV_OBJ_FLAG_HIDDEN);

        // Update action button text
        lv_label_set_text(ui_action_btn_label, "Change");

    } else {
        // Update icon color to gray/red
        lv_obj_set_style_text_color(ui_wifi_icon, lv_color_hex(COLOR_DISCONNECTED), LV_PART_MAIN);

        // Update status label
        wifi_connection_state_t state = wifi_get_state();
        if (state == WIFI_STATE_CONNECTING) {
            // Animate spinner during connection
            char connecting_text[32];
            snprintf(connecting_text, sizeof(connecting_text), "%s Connecting...",
                     SPINNER_FRAMES[s_spinner_frame % SPINNER_FRAME_COUNT]);
            s_spinner_frame++;
            lv_label_set_text(ui_status_label, connecting_text);
            lv_obj_set_style_text_color(ui_status_label, lv_color_hex(0xFFC107), LV_PART_MAIN);  // Yellow
        } else if (state == WIFI_STATE_FAILED) {
            lv_label_set_text(ui_status_label, LV_SYMBOL_CLOSE " Connection Failed");
            lv_obj_set_style_text_color(ui_status_label, lv_color_hex(COLOR_DISCONNECTED), LV_PART_MAIN);
        } else {
            lv_label_set_text(ui_status_label, "Not Connected");
            lv_obj_set_style_text_color(ui_status_label, lv_color_hex(COLOR_DISCONNECTED), LV_PART_MAIN);
        }

        // Clear connection details
        lv_label_set_text(ui_ssid_label, "No WiFi network connected");
        lv_label_set_text(ui_ip_label, "Tap below to connect");
        lv_label_set_text(ui_signal_label, "");

        // Show info container with disconnect message
        lv_obj_clear_flag(ui_info_container, LV_OBJ_FLAG_HIDDEN);

        // Update action button text
        lv_label_set_text(ui_action_btn_label, "Scan");
    }
}

/**
 * @brief Action button click handler
 */
static void action_btn_event_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Action button clicked - navigating to network list");

    // Navigate to the WiFi network list screen
    // Note: wifi_list_show() will acquire the LVGL lock internally,
    // but LVGL event callbacks run within the LVGL task context,
    // so we're already in a safe context for LVGL operations.
    wifi_list_show();
}

/**
 * @brief Back button click handler
 */
static void back_btn_event_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Back button clicked");
    wifi_setup_hide();
}

/**
 * @brief Timer callback for status refresh during connection
 *
 * Periodically refreshes the display to show connection progress
 * and animate the spinner during CONNECTING state.
 */
static void status_refresh_timer_cb(lv_timer_t *timer)
{
    // Check if we're still on the WiFi setup screen
    if (ui_Page_WiFi == NULL || lv_scr_act() != ui_Page_WiFi) {
        return;
    }

    // Refresh the status display
    update_wifi_status_display();

    // Check if connection completed (success or failure)
    wifi_connection_state_t state = wifi_get_state();
    if (state == WIFI_STATE_CONNECTED) {
        ESP_LOGI(TAG, "Connection successful, stopping refresh timer");
        stop_status_timer();
    } else if (state == WIFI_STATE_FAILED) {
        ESP_LOGI(TAG, "Connection failed, stopping refresh timer");
        stop_status_timer();
    }
}

/**
 * @brief Start the status refresh timer
 */
static void start_status_timer(void)
{
    if (s_status_refresh_timer == NULL) {
        s_status_refresh_timer = lv_timer_create(status_refresh_timer_cb, STATUS_REFRESH_INTERVAL_MS, NULL);
        ESP_LOGI(TAG, "Started status refresh timer");
    }
}

/**
 * @brief Stop the status refresh timer
 */
static void stop_status_timer(void)
{
    if (s_status_refresh_timer != NULL) {
        lv_timer_del(s_status_refresh_timer);
        s_status_refresh_timer = NULL;
        ESP_LOGI(TAG, "Stopped status refresh timer");
    }
}

// ============================================================================
// Public API
// ============================================================================

void wifi_setup_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi setup module");

    // Ensure WiFi scan module is initialized
    wifi_scan_init();

    // Create the WiFi setup screen (within LVGL lock)
    if (lvgl_port_lock(100)) {
        ui_Page_WiFi_screen_init();
        lvgl_port_unlock();
    }

    // Initialize the WiFi list module (for network selection screen)
    wifi_list_init();

    ESP_LOGI(TAG, "WiFi setup module initialized");
}

void wifi_setup_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing WiFi setup module");

    // Deinitialize the WiFi list module first
    wifi_list_deinit();

    if (!lvgl_port_lock(100)) {
        ESP_LOGW(TAG, "Could not acquire LVGL lock for deinit");
        return;
    }

    // Stop and delete status refresh timer
    stop_status_timer();

    if (ui_Page_WiFi != NULL) {
        lv_obj_del(ui_Page_WiFi);
        ui_Page_WiFi = NULL;
        ui_wifi_title = NULL;
        ui_wifi_icon = NULL;
        ui_status_label = NULL;
        ui_ssid_label = NULL;
        ui_ip_label = NULL;
        ui_signal_label = NULL;
        ui_action_btn = NULL;
        ui_action_btn_label = NULL;
        ui_back_btn = NULL;
        ui_back_btn_label = NULL;
        ui_info_container = NULL;
    }

    // Reset spinner frame
    s_spinner_frame = 0;

    lvgl_port_unlock();
    ESP_LOGI(TAG, "WiFi setup module deinitialized");
}

void wifi_setup_show(void)
{
    ESP_LOGI(TAG, "Showing WiFi setup screen");

    if (!lvgl_port_lock(500)) {
        ESP_LOGW(TAG, "Could not get LVGL lock for show");
        return;
    }

    // Ensure screen exists
    if (ui_Page_WiFi == NULL) {
        ESP_LOGI(TAG, "WiFi screen not created, creating now");
        ui_Page_WiFi_screen_init();
    }

    // Update the display with current WiFi status
    update_wifi_status_display();

    // Check if we need to open the page
    lv_obj_t *current = lv_scr_act();
    if (current != ui_Page_WiFi) {
        ESP_LOGI(TAG, "Opening WiFi page via lv_pm_open_page()");

        // Use page manager to show the screen
        lv_pm_open_page(g_main, &group_page_wifi, PM_ADD_OBJS_TO_GROUP,
                        &ui_Page_WiFi, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0,
                        &ui_Page_WiFi_screen_init);
    } else {
        ESP_LOGI(TAG, "Already on WiFi screen, just updating status");
    }

    // Reset spinner animation frame and start status refresh timer
    s_spinner_frame = 0;
    start_status_timer();

    lvgl_port_unlock();
}

void wifi_setup_hide(void)
{
    ESP_LOGI(TAG, "Hiding WiFi setup screen");

    if (!lvgl_port_lock(100)) {
        ESP_LOGW(TAG, "Could not get LVGL lock for hide");
        return;
    }

    // Stop status refresh timer
    stop_status_timer();

    // Use page manager to return to previous
    lv_pm_return_to_previous();

    lvgl_port_unlock();
}

bool wifi_setup_is_visible(void)
{
    if (!lvgl_port_lock(50)) {
        return false;
    }

    bool visible = (ui_Page_WiFi != NULL) && (lv_scr_act() == ui_Page_WiFi);

    lvgl_port_unlock();
    return visible;
}

void wifi_setup_refresh(void)
{
    if (!lvgl_port_lock(100)) {
        ESP_LOGW(TAG, "Could not get LVGL lock for refresh");
        return;
    }

    // Check visibility within the same lock to avoid double locking
    if (ui_Page_WiFi != NULL && lv_scr_act() == ui_Page_WiFi) {
        update_wifi_status_display();

        /* When the user comes back from wifi_list having kicked off a
         * reconnect (or, with the new multi-network store, an auto-reconnect
         * to a saved network), this screen may have already torn down its
         * status-refresh timer the last time we hit CONNECTED. Without it,
         * the spinner/labels would stay frozen until the user navigates
         * away and back. Restart the timer so the in-flight attempt animates
         * and the eventual CONNECTED/FAILED transition is captured. */
        if (wifi_get_state() == WIFI_STATE_CONNECTING && s_status_refresh_timer == NULL) {
            start_status_timer();
        }
    }

    lvgl_port_unlock();
}
