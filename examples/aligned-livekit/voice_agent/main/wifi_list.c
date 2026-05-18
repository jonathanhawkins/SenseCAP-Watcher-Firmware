/**
 * @file wifi_list.c
 * @brief WiFi network list UI implementation
 *
 * Displays a scrollable list of available WiFi networks using LVGL's
 * lv_list widget. Supports encoder navigation and touch interaction
 * on the 412x412 round LCD display.
 *
 * Follows the Page Manager pattern established in volume_control.c
 * and wifi_setup.c for consistent screen transitions.
 */

#include "wifi_list.h"
#include "wifi_scan.h"
#include "wifi_setup.h"
#include "wifi_password.h"
#include "pm.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "wifi_list";

// Screen dimensions (412x412 round display)
#define SCREEN_WIDTH  412
#define SCREEN_HEIGHT 412

// Colors matching the device theme (same as wifi_setup.c)
#define COLOR_BACKGROUND    0x1a1a1a   // Dark background
#define COLOR_CARD_BG       0x252525   // Card/container background
#define COLOR_LIST_BG       0x252525   // List background
#define COLOR_LIST_ITEM     0x303030   // List item background
#define COLOR_LIST_ITEM_PRESSED 0x404040  // List item pressed
#define COLOR_TEXT_PRIMARY  0xFFFFFF   // White text
#define COLOR_TEXT_SECONDARY 0x888888  // Gray text for secondary info
#define COLOR_ACCENT        0x8FC31F   // Green accent
#define COLOR_CONNECTED     0x4CAF50   // Green for connected status
#define COLOR_SIGNAL_GOOD   0x4CAF50   // Green signal
#define COLOR_SIGNAL_FAIR   0xFFC107   // Yellow/amber signal
#define COLOR_SIGNAL_WEAK   0xFF5722   // Red signal
#define COLOR_BUTTON_BG     0x333333   // Button background
#define COLOR_BUTTON_PRESSED 0x444444  // Button pressed state

// Maximum SSID display length (truncate if longer)
#define MAX_SSID_DISPLAY_LEN 18

// Scan refresh timer interval (check every 500ms)
#define SCAN_CHECK_INTERVAL_MS 500

// WiFi list screen objects
static lv_obj_t *ui_Page_WiFiList = NULL;       // Main list screen
static lv_obj_t *ui_list_title = NULL;          // "Select Network" title
static lv_obj_t *ui_network_list = NULL;        // LVGL list widget
static lv_obj_t *ui_scan_btn = NULL;            // "Scan Again" button
static lv_obj_t *ui_scan_btn_label = NULL;      // Scan button text
static lv_obj_t *ui_back_btn = NULL;            // Back button
static lv_obj_t *ui_back_btn_label = NULL;      // Back button text
static lv_obj_t *ui_scanning_label = NULL;      // "Scanning..." pulsing indicator
static lv_obj_t *ui_no_networks_label = NULL;   // "No networks found" label

// Group info for page manager
static GroupInfo group_page_wifi_list;

// Timer for checking scan completion
static lv_timer_t *s_scan_check_timer = NULL;

// Timer for checking connection status changes
static lv_timer_t *s_connection_check_timer = NULL;
static bool s_was_connected = false;  // Track previous connection state

// Selected network info
static char s_selected_ssid[WIFI_SSID_MAX_LEN] = {0};
static wifi_scan_auth_t s_selected_auth = WIFI_SCAN_AUTH_OPEN;

// Track if we're waiting for scan results
static bool s_scan_in_progress = false;

// Static buffer for scan results to avoid stack allocation in callbacks
// (stack overflow was causing device resets when allocating 800 bytes on LVGL task stack)
static wifi_scan_result_t s_network_results[WIFI_SCAN_MAX_AP];

// Forward declarations
static void ui_Page_WiFiList_screen_init(void);
static void populate_network_list(void);
static void network_item_event_cb(lv_event_t *e);
static void scan_btn_event_cb(lv_event_t *e);
static void back_btn_event_cb(lv_event_t *e);
static void scan_check_timer_cb(lv_timer_t *timer);
static void connection_check_timer_cb(lv_timer_t *timer);
static void show_scanning_state(bool is_scanning);
static void setup_focus_styling(lv_obj_t *obj);
static void wifi_list_show_internal(void);
static void wifi_list_hide_internal(void);

/**
 * @brief Get signal strength color based on RSSI
 */
static lv_color_t get_signal_color(int8_t rssi)
{
    if (rssi >= -60) return lv_color_hex(COLOR_SIGNAL_GOOD);
    if (rssi >= -75) return lv_color_hex(COLOR_SIGNAL_FAIR);
    return lv_color_hex(COLOR_SIGNAL_WEAK);
}

/**
 * @brief Get signal bars representation (1-4 bars)
 */
static const char* get_signal_bars(int8_t rssi)
{
    if (rssi >= -50) return "||||";  // Excellent
    if (rssi >= -60) return "|||";   // Good
    if (rssi >= -70) return "||";    // Fair
    if (rssi >= -80) return "|";     // Weak
    return ".";                       // Very weak
}

/**
 * @brief lv_anim exec callback: set LV_PART_MAIN opacity on the target object.
 *
 * Used to pulse the "Scanning..." label's brightness without moving any pixels
 * in space. lv_anim's exec_cb signature is (void *var, int32_t val) — it can't
 * accept the part selector that lv_obj_set_style_opa expects, hence this thin
 * wrapper.
 */
static void scanning_label_opa_cb(void *var, int32_t val)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)val, LV_PART_MAIN);
}

/**
 * @brief Setup focus styling for encoder navigation
 */
static void setup_focus_styling(lv_obj_t *obj)
{
    lv_obj_set_style_outline_color(obj, lv_color_hex(COLOR_ACCENT), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(obj, 3, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_opa(obj, 255, LV_PART_MAIN | LV_STATE_FOCUSED);
}

/**
 * @brief Create the WiFi list screen UI
 */
static void ui_Page_WiFiList_screen_init(void)
{
    if (ui_Page_WiFiList != NULL) {
        return;  // Already created
    }

    ESP_LOGI(TAG, "Creating WiFi list screen");

    // Create screen with dark background
    ui_Page_WiFiList = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_Page_WiFiList, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Page_WiFiList, lv_color_hex(COLOR_BACKGROUND), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_Page_WiFiList, 255, LV_PART_MAIN);

    // Title: "Select Network"
    ui_list_title = lv_label_create(ui_Page_WiFiList);
    lv_label_set_text(ui_list_title, "Select Network");
    lv_obj_set_style_text_color(ui_list_title, lv_color_hex(COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_list_title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(ui_list_title, LV_ALIGN_TOP_MID, 0, 40);

    // Scanning indicator: a static label whose opacity pulses.
    //
    // We previously used lv_spinner here, but on this board the rotating arc
    // TEARED badly. LVGL renders into a 40-line partial buffer per flush
    // (`CONFIG_LVGL_DRAW_BUFF_HEIGHT=40` in board.c — full 412-line buffer
    // exceeds available DMA SRAM). The 412 px display needs ~11 strip flushes
    // per frame; the rotating arc advances during the time it takes to push
    // each strip to the panel, so each horizontal strip captures the arc at a
    // slightly different angle. Stacked, that produces vertical comb-tooth
    // streaks radiating from the arc — fundamentally a partial-buffer + moving
    // thin geometry artifact, not an LVGL config issue.
    //
    // Fix: don't move ANY pixels spatially. A label that never changes (x, y)
    // can't tear; only the alpha changes between frames. To still convey
    // "I'm working," pulse the opacity (60% → 100% → 60% over 2 s).
    ui_scanning_label = lv_label_create(ui_Page_WiFiList);
    lv_label_set_text(ui_scanning_label, "Scanning...");
    lv_obj_set_style_text_color(ui_scanning_label, lv_color_hex(COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_scanning_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(ui_scanning_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(ui_scanning_label, LV_OBJ_FLAG_HIDDEN);

    // No networks found label (hidden by default)
    ui_no_networks_label = lv_label_create(ui_Page_WiFiList);
    // Hint the most common phone-hotspot footgun. iPhone defaults to WPA3-only
    // unless Maximize Compatibility is on; many ESP32 boards (and a lot of
    // older WPA2-only clients) can't see those networks during a scan.
    lv_label_set_text(ui_no_networks_label,
                      "No networks found.\nIf using iPhone hotspot,\nturn on Maximize Compatibility.");
    lv_obj_set_style_text_color(ui_no_networks_label, lv_color_hex(COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_no_networks_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(ui_no_networks_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(ui_no_networks_label, 280);  // Fit comfortably inside round display
    lv_obj_align(ui_no_networks_label, LV_ALIGN_CENTER, 0, -20);
    lv_obj_add_flag(ui_no_networks_label, LV_OBJ_FLAG_HIDDEN);

    // Create scrollable list container
    // Position it within the circular display area (leave margins for round edges)
    ui_network_list = lv_list_create(ui_Page_WiFiList);
    lv_obj_set_size(ui_network_list, 300, 180);  // Sized for round display
    lv_obj_align(ui_network_list, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_bg_color(ui_network_list, lv_color_hex(COLOR_LIST_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_network_list, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(ui_network_list, 16, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_network_list, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_network_list, lv_color_hex(0x404040), LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui_network_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ui_network_list, 4, LV_PART_MAIN);

    // Style the scrollbar
    lv_obj_set_style_bg_color(ui_network_list, lv_color_hex(COLOR_ACCENT), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(ui_network_list, 200, LV_PART_SCROLLBAR);

    // Bottom button container
    lv_obj_t *btn_container = lv_obj_create(ui_Page_WiFiList);
    lv_obj_set_size(btn_container, 280, 50);
    lv_obj_align(btn_container, LV_ALIGN_BOTTOM_MID, 0, -45);
    lv_obj_set_flex_flow(btn_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_container, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(btn_container, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_container, 0, LV_PART_MAIN);

    // "Scan Again" button
    ui_scan_btn = lv_btn_create(btn_container);
    lv_obj_set_size(ui_scan_btn, 120, 40);
    lv_obj_set_style_bg_color(ui_scan_btn, lv_color_hex(COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_scan_btn, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(ui_scan_btn, 20, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ui_scan_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_scan_btn, lv_color_hex(0x7AAE1A), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(ui_scan_btn, scan_btn_event_cb, LV_EVENT_CLICKED, NULL);
    setup_focus_styling(ui_scan_btn);

    ui_scan_btn_label = lv_label_create(ui_scan_btn);
    lv_label_set_text(ui_scan_btn_label, LV_SYMBOL_REFRESH " Scan");
    lv_obj_set_style_text_color(ui_scan_btn_label, lv_color_hex(COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_scan_btn_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(ui_scan_btn_label);

    // "Back" button
    ui_back_btn = lv_btn_create(btn_container);
    lv_obj_set_size(ui_back_btn, 100, 40);
    lv_obj_set_style_bg_color(ui_back_btn, lv_color_hex(COLOR_BUTTON_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_back_btn, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(ui_back_btn, 20, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ui_back_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_back_btn, lv_color_hex(COLOR_BUTTON_PRESSED), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(ui_back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);
    setup_focus_styling(ui_back_btn);

    ui_back_btn_label = lv_label_create(ui_back_btn);
    lv_label_set_text(ui_back_btn_label, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(ui_back_btn_label, lv_color_hex(COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_back_btn_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(ui_back_btn_label);

    // Setup group info for encoder navigation
    // The list itself will be navigable via encoder, plus the two buttons
    group_page_wifi_list.obj_count = 3;
    group_page_wifi_list.group[0] = ui_network_list;
    group_page_wifi_list.group[1] = ui_scan_btn;
    group_page_wifi_list.group[2] = ui_back_btn;

    ESP_LOGI(TAG, "WiFi list screen created: %p", ui_Page_WiFiList);
}

/**
 * @brief Show scanning state (show/hide scanning indicator)
 */
static void show_scanning_state(bool is_scanning)
{
    if (ui_Page_WiFiList == NULL) return;

    s_scan_in_progress = is_scanning;

    if (is_scanning) {
        lv_obj_clear_flag(ui_scanning_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_network_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_no_networks_label, LV_OBJ_FLAG_HIDDEN);

        // Start (or restart) the opacity-pulse animation. LVGL anims live on
        // the global anim list keyed by (var, exec_cb), so a duplicate start
        // updates the existing one rather than stacking — safe to call from
        // any scanning-state transition.
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ui_scanning_label);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)scanning_label_opa_cb);
        lv_anim_set_values(&a, LV_OPA_60, LV_OPA_COVER);
        lv_anim_set_time(&a, 1000);
        lv_anim_set_playback_time(&a, 1000);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);

        // Disable scan button during scan
        lv_obj_add_state(ui_scan_btn, LV_STATE_DISABLED);
        lv_label_set_text(ui_scan_btn_label, "Scanning...");
    } else {
        // Stop the pulse animation so the LVGL anim list doesn't keep firing
        // against a hidden widget (cheap but pointless).
        lv_anim_del(ui_scanning_label, (lv_anim_exec_xcb_t)scanning_label_opa_cb);
        lv_obj_add_flag(ui_scanning_label, LV_OBJ_FLAG_HIDDEN);

        // Re-enable scan button
        lv_obj_clear_state(ui_scan_btn, LV_STATE_DISABLED);
        lv_label_set_text(ui_scan_btn_label, LV_SYMBOL_REFRESH " Scan");
    }
}

/**
 * @brief Populate the network list with scan results
 */
static void populate_network_list(void)
{
    if (ui_network_list == NULL) {
        ESP_LOGW(TAG, "Network list widget is NULL");
        return;
    }

    // Clear existing items
    lv_obj_clean(ui_network_list);

    // Get scan results (thread-safe copy) - use static buffer to avoid stack overflow
    uint16_t count = wifi_scan_copy_results(s_network_results, WIFI_SCAN_MAX_AP);

    ESP_LOGI(TAG, "Populating list with %d networks", count);

    if (count == 0) {
        // No networks found
        lv_obj_add_flag(ui_network_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_no_networks_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // Show the list, hide the no-networks label
    lv_obj_clear_flag(ui_network_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_no_networks_label, LV_OBJ_FLAG_HIDDEN);

    // Track items for group navigation
    int item_count = 0;

    /* Cap UI rows below the buffer size: rendering more LVGL list buttons
     * than this risks exhausting the LVGL heap and wedging input handling.
     * The buffer (WIFI_SCAN_MAX_AP) is already RSSI-sorted, so the phone
     * hotspot (close radio = strong signal) sorts into these top rows. */
    const uint16_t kMaxRenderedNetworks = 15;
    for (uint16_t i = 0; i < count && item_count < kMaxRenderedNetworks; i++) {
        // Skip hidden/empty SSIDs
        if (s_network_results[i].ssid[0] == '\0') {
            continue;
        }

        // Build display string: SSID + signal + lock
        char display_text[64];
        char ssid_truncated[MAX_SSID_DISPLAY_LEN + 1];

        // Truncate SSID if needed
        size_t ssid_len = strlen(s_network_results[i].ssid);
        if (ssid_len > MAX_SSID_DISPLAY_LEN) {
            strncpy(ssid_truncated, s_network_results[i].ssid, MAX_SSID_DISPLAY_LEN - 2);
            ssid_truncated[MAX_SSID_DISPLAY_LEN - 2] = '.';
            ssid_truncated[MAX_SSID_DISPLAY_LEN - 1] = '.';
            ssid_truncated[MAX_SSID_DISPLAY_LEN] = '\0';
        } else {
            strncpy(ssid_truncated, s_network_results[i].ssid, sizeof(ssid_truncated) - 1);
            ssid_truncated[sizeof(ssid_truncated) - 1] = '\0';
        }

        // Build the trailing-symbol suffix for disconnected rows.
        // - Secured + we have a saved password    -> [save][lock]  ("tap and you're in")
        // - Secured + no saved password           -> [lock]        (today's default)
        // - Open                                  -> ""            (no password ever needed)
        // For the connected row we still use a single OK checkmark — both saved
        // and not-saved states are subsumed by "you're currently on it".
        // LV_SYMBOL_EYE_CLOSE serves as the lock glyph (closest in the LVGL font).
        const bool is_secured = (s_network_results[i].authmode != WIFI_SCAN_AUTH_OPEN);
        const bool is_saved = is_secured && wifi_has_saved_for_ssid(s_network_results[i].ssid);
        const char *suffix;
        if (!is_secured) {
            suffix = "";
        } else if (is_saved) {
            suffix = LV_SYMBOL_SAVE " " LV_SYMBOL_EYE_CLOSE;
        } else {
            suffix = LV_SYMBOL_EYE_CLOSE;
        }

        if (s_network_results[i].is_connected) {
            // Connected network - show checkmark and "Connected"
            snprintf(display_text, sizeof(display_text), "%s %s " LV_SYMBOL_OK,
                     LV_SYMBOL_WIFI,
                     ssid_truncated);
        } else {
            snprintf(display_text, sizeof(display_text), "%s %s %ddB %s",
                     LV_SYMBOL_WIFI,
                     ssid_truncated,
                     s_network_results[i].rssi,
                     suffix);
        }

        // Create list button
        lv_obj_t *btn = lv_list_add_btn(ui_network_list, NULL, display_text);
        if (btn == NULL) {
            /* LVGL heap exhausted — stop adding rows rather than dereference
             * NULL in the styling/focus-group calls below, which would wedge
             * the encoder input handler. */
            ESP_LOGW(TAG, "LVGL alloc failed at row %d; truncating list", item_count);
            break;
        }

        // Store result index as user data for callback
        lv_obj_set_user_data(btn, (void*)(uintptr_t)i);

        // Style the button
        lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_LIST_ITEM), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, 255, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(btn, 8, LV_PART_MAIN);

        // Pressed state
        lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_LIST_ITEM_PRESSED), LV_PART_MAIN | LV_STATE_PRESSED);

        // Get the label child and style it
        lv_obj_t *label = lv_obj_get_child(btn, 0);
        if (label) {
            lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);

            // Highlight connected network
            if (s_network_results[i].is_connected) {
                lv_obj_set_style_text_color(label, lv_color_hex(COLOR_CONNECTED), LV_PART_MAIN);
                lv_obj_set_style_bg_color(btn, lv_color_hex(0x1a3d1a), LV_PART_MAIN);  // Dark green bg
            } else {
                // Color based on signal strength
                lv_obj_set_style_text_color(label, get_signal_color(s_network_results[i].rssi), LV_PART_MAIN);
            }
        }

        // Focus styling for encoder navigation
        setup_focus_styling(btn);

        // Add click event (index stored in user_data via lv_obj_set_user_data above)
        lv_obj_add_event_cb(btn, network_item_event_cb, LV_EVENT_CLICKED, NULL);

        item_count++;
    }

    ESP_LOGI(TAG, "Added %d network items to list", item_count);
}

/**
 * @brief Network item click handler
 */
static void network_item_event_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);

    // Get result index from user data
    uintptr_t index = (uintptr_t)lv_obj_get_user_data(btn);

    // Use the static network results buffer (already populated by populate_network_list)
    // This avoids stack allocation which was causing device resets
    uint16_t count = wifi_scan_get_count();

    if (index >= count) {
        ESP_LOGW(TAG, "Invalid network index: %lu", (unsigned long)index);
        return;
    }

    // Store selected network info from the static buffer
    strncpy(s_selected_ssid, s_network_results[index].ssid, WIFI_SSID_MAX_LEN - 1);
    s_selected_ssid[WIFI_SSID_MAX_LEN - 1] = '\0';
    s_selected_auth = s_network_results[index].authmode;

    ESP_LOGI(TAG, "Network selected: '%s' (auth=%d, rssi=%d)",
             s_selected_ssid, s_selected_auth, s_network_results[index].rssi);

    if (s_selected_auth == WIFI_SCAN_AUTH_OPEN) {
        // Open network - connect directly
        ESP_LOGI(TAG, "Open network, connecting directly...");

        // Start connection
        if (wifi_connect(s_selected_ssid, NULL)) {
            ESP_LOGI(TAG, "Connection initiated to open network");
            // Return to WiFi status screen to show connection progress
            wifi_list_hide_internal();
        } else {
            ESP_LOGE(TAG, "Failed to initiate connection");
        }
        return;
    }

    /* Secured network — try to reconnect using stored credentials before
     * dragging the user through the on-screen keyboard again. The lookup is
     * a single NVS read so it's cheap to do inline. */
    char saved_pass[WIFI_PASSWORD_MAX_LEN];
    if (wifi_get_saved_password(s_selected_ssid, saved_pass, sizeof(saved_pass))) {
        ESP_LOGI(TAG, "Found saved password for '%s', auto-connecting", s_selected_ssid);
        if (wifi_connect(s_selected_ssid, saved_pass)) {
            /* Pop back to wifi_setup; its refresh hook will show the
             * connecting/connected state. */
            wifi_list_hide_internal();
        } else {
            ESP_LOGE(TAG, "Failed to initiate auto-connect, falling back to password entry");
            wifi_password_show(s_selected_ssid);
        }
        /* Wipe the local password copy ASAP. */
        memset(saved_pass, 0, sizeof(saved_pass));
        return;
    }

    // No saved credentials — navigate to password entry screen
    ESP_LOGI(TAG, "Secured network, no saved password, navigating to password entry");
    wifi_password_show(s_selected_ssid);
}

/**
 * @brief Scan button click handler
 */
static void scan_btn_event_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Scan button clicked");

    // Start a new scan
    wifi_scan_start();
    show_scanning_state(true);
}

/**
 * @brief Back button click handler
 */
static void back_btn_event_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Back button clicked");
    wifi_list_hide_internal();
}

/**
 * @brief Timer callback to check scan completion
 */
static void scan_check_timer_cb(lv_timer_t *timer)
{
    if (!s_scan_in_progress) {
        return;
    }

    if (wifi_scan_is_complete()) {
        ESP_LOGI(TAG, "Scan completed, refreshing list");
        show_scanning_state(false);
        populate_network_list();
    }
}

/**
 * @brief Timer callback to check connection status changes
 *
 * When WiFi connects, we need to refresh the list to show the
 * "Connected" indicator next to the network name.
 */
static void connection_check_timer_cb(lv_timer_t *timer)
{
    bool is_connected = wifi_is_connected();

    // Detect state change: just connected
    if (is_connected && !s_was_connected) {
        ESP_LOGI(TAG, "WiFi connected! Refreshing list to show connected status");
        s_was_connected = true;

        // Need to re-scan to get fresh results with is_connected flag set
        // The wifi_scan module updates is_connected during process_scan_results()
        // But we can't scan while connected - just refresh the display
        // The is_connected flag is checked against s_current_ssid in wifi_scan.c

        // Do a quick re-scan to refresh the list with connected status
        if (!s_scan_in_progress) {
            wifi_scan_start();
            show_scanning_state(true);
        }
    }
    // Detect state change: just disconnected
    else if (!is_connected && s_was_connected) {
        ESP_LOGI(TAG, "WiFi disconnected, refreshing list");
        s_was_connected = false;

        // Refresh to clear connected indicator
        if (!s_scan_in_progress) {
            wifi_scan_start();
            show_scanning_state(true);
        }
    }
}

// ============================================================================
// Public API
// ============================================================================

void wifi_list_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi list module");

    // Ensure WiFi scan module is initialized
    wifi_scan_init();

    // Initialize password entry module (for secured networks)
    wifi_password_init();

    // Create the WiFi list screen (within LVGL lock)
    if (lvgl_port_lock(100)) {
        ui_Page_WiFiList_screen_init();
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "WiFi list module initialized");
}

void wifi_list_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing WiFi list module");

    if (!lvgl_port_lock(100)) {
        ESP_LOGW(TAG, "Could not acquire LVGL lock for deinit");
        return;
    }

    // Delete scan check timer
    if (s_scan_check_timer != NULL) {
        lv_timer_del(s_scan_check_timer);
        s_scan_check_timer = NULL;
    }

    // Delete connection check timer
    if (s_connection_check_timer != NULL) {
        lv_timer_del(s_connection_check_timer);
        s_connection_check_timer = NULL;
    }

    // Delete the screen
    if (ui_Page_WiFiList != NULL) {
        lv_obj_del(ui_Page_WiFiList);
        ui_Page_WiFiList = NULL;
        ui_list_title = NULL;
        ui_network_list = NULL;
        ui_scan_btn = NULL;
        ui_scan_btn_label = NULL;
        ui_back_btn = NULL;
        ui_back_btn_label = NULL;
        ui_scanning_label = NULL;
        ui_no_networks_label = NULL;
    }

    // Clear selected network
    s_selected_ssid[0] = '\0';
    s_selected_auth = WIFI_SCAN_AUTH_OPEN;

    lvgl_port_unlock();
    ESP_LOGI(TAG, "WiFi list module deinitialized");
}

/**
 * @brief Internal show function - assumes LVGL lock is already held
 *
 * Called from LVGL event callbacks where we're already in the LVGL context
 */
static void wifi_list_show_internal(void)
{
    // Ensure screen exists
    if (ui_Page_WiFiList == NULL) {
        ESP_LOGI(TAG, "WiFi list screen not created, creating now");
        ui_Page_WiFiList_screen_init();
    }

    // Start a WiFi scan
    wifi_scan_start();
    show_scanning_state(true);

    // Create scan check timer if not exists
    if (s_scan_check_timer == NULL) {
        s_scan_check_timer = lv_timer_create(scan_check_timer_cb, SCAN_CHECK_INTERVAL_MS, NULL);
    }

    // Create connection check timer to detect when WiFi connects
    if (s_connection_check_timer == NULL) {
        s_was_connected = wifi_is_connected();  // Initialize current state
        s_connection_check_timer = lv_timer_create(connection_check_timer_cb, 1000, NULL);  // Check every second
    }

    // Check if we need to open the page
    lv_obj_t *current = lv_scr_act();
    if (current != ui_Page_WiFiList) {
        ESP_LOGI(TAG, "Opening WiFi list page via lv_pm_open_page()");

        // Use page manager to show the screen
        lv_pm_open_page(g_main, &group_page_wifi_list, PM_ADD_OBJS_TO_GROUP,
                        &ui_Page_WiFiList, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0,
                        &ui_Page_WiFiList_screen_init);
    } else {
        ESP_LOGI(TAG, "Already on WiFi list screen");
    }
}

/**
 * @brief Internal hide function - assumes LVGL lock is already held
 *
 * Called from LVGL event callbacks where we're already in the LVGL context
 */
static void wifi_list_hide_internal(void)
{
    // Stop scan check timer
    if (s_scan_check_timer != NULL) {
        lv_timer_del(s_scan_check_timer);
        s_scan_check_timer = NULL;
    }

    // Stop connection check timer
    if (s_connection_check_timer != NULL) {
        lv_timer_del(s_connection_check_timer);
        s_connection_check_timer = NULL;
    }

    s_scan_in_progress = false;

    // Use page manager to return to previous
    lv_pm_return_to_previous();

    /* The WiFi setup screen's status-refresh timer self-stops once the
     * device reaches WIFI_STATE_CONNECTED (it's intended for the connecting
     * spinner, not as a live display). lv_pm_return_to_previous() merely
     * swaps the active LVGL screen — it does not re-run our show callback,
     * so the SSID / IP / RSSI labels would otherwise stay frozen at the
     * value they had when the user first entered the screen.
     * Explicitly refresh here so reconnecting to a new AP updates the UI. */
    wifi_setup_refresh();
}

void wifi_list_show(void)
{
    ESP_LOGI(TAG, "Showing WiFi list screen");

    // Try to acquire LVGL lock (uses recursive mutex, safe if already held)
    bool acquired_lock = lvgl_port_lock(500);
    if (!acquired_lock) {
        ESP_LOGW(TAG, "Could not get LVGL lock for show");
        return;
    }

    wifi_list_show_internal();

    lvgl_port_unlock();
}

void wifi_list_hide(void)
{
    ESP_LOGI(TAG, "Hiding WiFi list screen");

    bool acquired_lock = lvgl_port_lock(100);
    if (!acquired_lock) {
        ESP_LOGW(TAG, "Could not get LVGL lock for hide");
        return;
    }

    wifi_list_hide_internal();

    lvgl_port_unlock();
}

bool wifi_list_is_visible(void)
{
    if (!lvgl_port_lock(50)) {
        return false;
    }

    bool visible = (ui_Page_WiFiList != NULL) && (lv_scr_act() == ui_Page_WiFiList);

    lvgl_port_unlock();
    return visible;
}

void wifi_list_refresh(void)
{
    if (!lvgl_port_lock(100)) {
        ESP_LOGW(TAG, "Could not get LVGL lock for refresh");
        return;
    }

    // Check visibility within the same lock
    if (ui_Page_WiFiList != NULL && lv_scr_act() == ui_Page_WiFiList) {
        if (!s_scan_in_progress) {
            populate_network_list();
        }
    }

    lvgl_port_unlock();
}

const char* wifi_list_get_selected_ssid(void)
{
    return s_selected_ssid;
}

bool wifi_list_selected_needs_password(void)
{
    return (s_selected_auth != WIFI_SCAN_AUTH_OPEN);
}
