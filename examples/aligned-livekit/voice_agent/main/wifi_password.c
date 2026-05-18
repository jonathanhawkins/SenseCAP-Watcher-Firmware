/**
 * @file wifi_password.c
 * @brief WiFi password entry UI implementation
 *
 * Full on-screen QWERTY keyboard for entering WiFi passwords.
 * Designed for 412x412 round LCD display with touch and encoder support.
 */

#include "wifi_password.h"
#include "wifi_scan.h"
#include "wifi_setup.h"
#include "wifi_list.h"
#include "pm.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include <string.h>

static const char *TAG = "wifi_password";

// Screen dimensions
#define SCREEN_WIDTH  412
#define SCREEN_HEIGHT 412

// Colors matching the device theme
#define COLOR_BACKGROUND    0x1a1a1a   // Dark background
#define COLOR_CARD_BG       0x252525   // Card/container background
#define COLOR_TEXT_PRIMARY  0xFFFFFF   // White text
#define COLOR_TEXT_SECONDARY 0x888888  // Gray text
#define COLOR_ACCENT        0x8FC31F   // Green accent
#define COLOR_BUTTON_BG     0x333333   // Button background
#define COLOR_BUTTON_PRESSED 0x444444  // Button pressed
#define COLOR_KB_BG         0x303030   // Keyboard background
#define COLOR_KB_KEY        0x404040   // Key background
#define COLOR_KB_KEY_PRESSED 0x505050  // Key pressed
#define COLOR_ERROR         0xFF5722   // Error red

// Maximum password length
#define MAX_PASSWORD_LEN 63

// Password entry screen objects
static lv_obj_t *ui_Page_Password = NULL;        // Main password screen
static lv_obj_t *ui_ssid_label = NULL;           // "Network: SSID" label
static lv_obj_t *ui_password_ta = NULL;          // Password text area
static lv_obj_t *ui_show_password_cb = NULL;     // Show password checkbox
static lv_obj_t *ui_keyboard = NULL;             // LVGL keyboard widget
static lv_obj_t *ui_connect_btn = NULL;          // Connect button
static lv_obj_t *ui_cancel_btn = NULL;           // Cancel button
static lv_obj_t *ui_error_label = NULL;          // Error message label

// Group info for page manager
static GroupInfo group_page_password;

// Network info
static char s_target_ssid[33] = {0};
static char s_password[MAX_PASSWORD_LEN + 1] = {0};

// Timer that polls wifi state while the password screen is up so we can
// surface a meaningful error (Wrong password / Out of range / etc.) on the
// existing ui_error_label instead of bouncing back to a generic "Connection
// Failed" status on the previous screen.
static lv_timer_t *s_state_watch_timer = NULL;
#define STATE_WATCH_INTERVAL_MS 250

// Forward decl
static void state_watch_timer_cb(lv_timer_t *timer);
static void stop_state_watch(void);
static const char *describe_disconnect_reason(int reason, char *buf, size_t buflen);

// Forward declarations
static void ui_Page_Password_screen_init(void);
static void keyboard_event_cb(lv_event_t *e);
static void text_area_event_cb(lv_event_t *e);
static void show_password_event_cb(lv_event_t *e);
static void connect_btn_event_cb(lv_event_t *e);
static void cancel_btn_event_cb(lv_event_t *e);
static void setup_focus_styling(lv_obj_t *obj);
static void wifi_password_show_internal(void);
static void wifi_password_hide_internal(void);
static void attempt_connection(void);

/**
 * @brief Setup focus styling for encoder navigation
 */
static void setup_focus_styling(lv_obj_t *obj)
{
    lv_obj_set_style_outline_color(obj, lv_color_hex(COLOR_ACCENT), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(obj, 2, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_opa(obj, 255, LV_PART_MAIN | LV_STATE_FOCUSED);
}

/**
 * @brief Create the password entry screen UI
 */
static void ui_Page_Password_screen_init(void)
{
    if (ui_Page_Password != NULL) {
        return;  // Already created
    }

    ESP_LOGI(TAG, "Creating password entry screen");

    // Create screen with dark background
    ui_Page_Password = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_Page_Password, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Page_Password, lv_color_hex(COLOR_BACKGROUND), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_Page_Password, 255, LV_PART_MAIN);

    // Network SSID label at top
    ui_ssid_label = lv_label_create(ui_Page_Password);
    lv_label_set_text(ui_ssid_label, "Network: ---");
    lv_obj_set_style_text_color(ui_ssid_label, lv_color_hex(COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_ssid_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(ui_ssid_label, LV_ALIGN_TOP_MID, 0, 35);

    // Password text area
    ui_password_ta = lv_textarea_create(ui_Page_Password);
    lv_obj_set_size(ui_password_ta, 280, 44);
    lv_obj_align(ui_password_ta, LV_ALIGN_TOP_MID, 0, 60);
    lv_textarea_set_placeholder_text(ui_password_ta, "Enter password");
    lv_textarea_set_password_mode(ui_password_ta, true);
    lv_textarea_set_max_length(ui_password_ta, MAX_PASSWORD_LEN);
    lv_textarea_set_one_line(ui_password_ta, true);

    // Style the text area
    lv_obj_set_style_bg_color(ui_password_ta, lv_color_hex(COLOR_CARD_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_password_ta, 255, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_password_ta, lv_color_hex(COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_password_ta, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_password_ta, lv_color_hex(COLOR_ACCENT), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ui_password_ta, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_password_ta, lv_color_hex(0x404040), LV_PART_MAIN);
    lv_obj_set_style_radius(ui_password_ta, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui_password_ta, 8, LV_PART_MAIN);

    // Cursor styling
    lv_obj_set_style_bg_color(ui_password_ta, lv_color_hex(COLOR_ACCENT), LV_PART_CURSOR);

    lv_obj_add_event_cb(ui_password_ta, text_area_event_cb, LV_EVENT_FOCUSED, NULL);

    // Show password checkbox (styled as a small button)
    ui_show_password_cb = lv_checkbox_create(ui_Page_Password);
    lv_checkbox_set_text(ui_show_password_cb, "Show");
    lv_obj_set_style_text_color(ui_show_password_cb, lv_color_hex(COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_show_password_cb, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(ui_show_password_cb, LV_ALIGN_TOP_RIGHT, -60, 108);

    // Style the checkbox indicator
    lv_obj_set_style_border_color(ui_show_password_cb, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui_show_password_cb, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);

    lv_obj_add_event_cb(ui_show_password_cb, show_password_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Error label (hidden by default)
    ui_error_label = lv_label_create(ui_Page_Password);
    lv_label_set_text(ui_error_label, "");
    lv_obj_set_style_text_color(ui_error_label, lv_color_hex(COLOR_ERROR), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_error_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(ui_error_label, LV_ALIGN_TOP_LEFT, 66, 108);
    lv_obj_add_flag(ui_error_label, LV_OBJ_FLAG_HIDDEN);

    // Create keyboard - sized to fit the lower portion of round display
    ui_keyboard = lv_keyboard_create(ui_Page_Password);
    lv_obj_set_size(ui_keyboard, 320, 165);
    lv_obj_align(ui_keyboard, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_keyboard_set_textarea(ui_keyboard, ui_password_ta);

    // Style the keyboard
    lv_obj_set_style_bg_color(ui_keyboard, lv_color_hex(COLOR_KB_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_keyboard, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(ui_keyboard, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_keyboard, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui_keyboard, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(ui_keyboard, 3, LV_PART_MAIN);

    // Style the keys
    lv_obj_set_style_bg_color(ui_keyboard, lv_color_hex(COLOR_KB_KEY), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(ui_keyboard, 255, LV_PART_ITEMS);
    lv_obj_set_style_text_color(ui_keyboard, lv_color_hex(COLOR_TEXT_PRIMARY), LV_PART_ITEMS);
    lv_obj_set_style_text_font(ui_keyboard, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_radius(ui_keyboard, 6, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(ui_keyboard, 0, LV_PART_ITEMS);

    // Pressed key styling
    lv_obj_set_style_bg_color(ui_keyboard, lv_color_hex(COLOR_KB_KEY_PRESSED), LV_PART_ITEMS | LV_STATE_PRESSED);

    // Checked (active mode) styling for mode switch keys
    lv_obj_set_style_bg_color(ui_keyboard, lv_color_hex(COLOR_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);

    // Register for specific keyboard events only (more efficient than LV_EVENT_ALL)
    lv_obj_add_event_cb(ui_keyboard, keyboard_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(ui_keyboard, keyboard_event_cb, LV_EVENT_CANCEL, NULL);

    // Button container at bottom
    lv_obj_t *btn_container = lv_obj_create(ui_Page_Password);
    lv_obj_set_size(btn_container, 280, 45);
    lv_obj_align(btn_container, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_obj_set_flex_flow(btn_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_container, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(btn_container, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_container, 0, LV_PART_MAIN);

    // Connect button
    ui_connect_btn = lv_btn_create(btn_container);
    lv_obj_set_size(ui_connect_btn, 120, 36);
    lv_obj_set_style_bg_color(ui_connect_btn, lv_color_hex(COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_connect_btn, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(ui_connect_btn, 18, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ui_connect_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_connect_btn, lv_color_hex(0x7AAE1A), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(ui_connect_btn, connect_btn_event_cb, LV_EVENT_CLICKED, NULL);
    setup_focus_styling(ui_connect_btn);

    lv_obj_t *connect_label = lv_label_create(ui_connect_btn);
    lv_label_set_text(connect_label, LV_SYMBOL_OK " Connect");
    lv_obj_set_style_text_color(connect_label, lv_color_hex(COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(connect_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(connect_label);

    // Cancel button
    ui_cancel_btn = lv_btn_create(btn_container);
    lv_obj_set_size(ui_cancel_btn, 100, 36);
    lv_obj_set_style_bg_color(ui_cancel_btn, lv_color_hex(COLOR_BUTTON_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_cancel_btn, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(ui_cancel_btn, 18, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ui_cancel_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_cancel_btn, lv_color_hex(COLOR_BUTTON_PRESSED), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(ui_cancel_btn, cancel_btn_event_cb, LV_EVENT_CLICKED, NULL);
    setup_focus_styling(ui_cancel_btn);

    lv_obj_t *cancel_label = lv_label_create(ui_cancel_btn);
    lv_label_set_text(cancel_label, LV_SYMBOL_CLOSE " Cancel");
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(cancel_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(cancel_label);

    // Setup group info for encoder navigation
    // Text area, keyboard, connect button, cancel button
    group_page_password.obj_count = 4;
    group_page_password.group[0] = ui_password_ta;
    group_page_password.group[1] = ui_keyboard;
    group_page_password.group[2] = ui_connect_btn;
    group_page_password.group[3] = ui_cancel_btn;

    ESP_LOGI(TAG, "Password entry screen created: %p", ui_Page_Password);
}

/**
 * @brief Keyboard event handler
 * @note Called from LVGL context - lock is already held by LVGL task
 */
static void keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_READY) {
        // User pressed OK/Enter on keyboard
        ESP_LOGI(TAG, "Keyboard READY event (Enter pressed)");
        attempt_connection();
    } else if (code == LV_EVENT_CANCEL) {
        // User pressed cancel/close on keyboard
        ESP_LOGI(TAG, "Keyboard CANCEL event");
        wifi_password_hide_internal();
    }
}

/**
 * @brief Text area event handler
 * @note Called from LVGL context - lock is already held by LVGL task
 */
static void text_area_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_FOCUSED) {
        // When text area is focused, also focus keyboard
        if (ui_keyboard != NULL) {
            lv_keyboard_set_textarea(ui_keyboard, ui_password_ta);
        }
    }
}

/**
 * @brief Show password checkbox handler
 * @note Called from LVGL context - lock is already held by LVGL task
 */
static void show_password_event_cb(lv_event_t *e)
{
    lv_obj_t *cb = lv_event_get_target(e);
    bool checked = lv_obj_has_state(cb, LV_STATE_CHECKED);

    ESP_LOGI(TAG, "Show password: %s", checked ? "ON" : "OFF");

    if (ui_password_ta != NULL) {
        lv_textarea_set_password_mode(ui_password_ta, !checked);
    }
}

/**
 * @brief Map ESP-IDF WiFi disconnect reason to a short user-facing string.
 *
 * Codes from `esp_wifi_types_generic.h`. We only map the ones the user is
 * likely to hit in the field; everything else falls through to a generic
 * "Failed (reason N)" so we can debug without renaming firmware codes.
 */
static const char *describe_disconnect_reason(int reason, char *buf, size_t buflen)
{
    switch (reason) {
        case 2:   /* AUTH_EXPIRE */
        case 15:  /* 4WAY_HANDSHAKE_TIMEOUT */
            return "Wrong password";
        case 201: /* NO_AP_FOUND */
            return "Network out of range";
        case 202: /* AUTH_FAIL */
            return "Auth failed - try again";
        case 205: /* CONNECTION_FAIL */
            return "Hotspot disconnected";
        default:
            snprintf(buf, buflen, "Failed (reason %d)", reason);
            return buf;
    }
}

static void stop_state_watch(void)
{
    if (s_state_watch_timer != NULL) {
        lv_timer_del(s_state_watch_timer);
        s_state_watch_timer = NULL;
    }
}

/**
 * @brief Periodic check while password screen is up — show error or dismiss.
 *
 * Called from LVGL task context, so we can touch widgets directly.
 */
static void state_watch_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    // Bail if the screen got torn down underneath us.
    if (ui_Page_Password == NULL || lv_scr_act() != ui_Page_Password) {
        stop_state_watch();
        return;
    }

    wifi_connection_state_t state = wifi_get_state();
    if (state == WIFI_STATE_CONNECTED) {
        ESP_LOGI(TAG, "Connection succeeded, dismissing password screen");
        stop_state_watch();
        wifi_password_hide_internal();
    } else if (state == WIFI_STATE_FAILED) {
        int reason = wifi_get_last_disconnect_reason();
        char fallback[24];
        const char *msg = describe_disconnect_reason(reason, fallback, sizeof(fallback));
        ESP_LOGW(TAG, "Connection failed (reason %d): %s", reason, msg);

        if (ui_error_label != NULL) {
            lv_label_set_text(ui_error_label, msg);
            lv_obj_clear_flag(ui_error_label, LV_OBJ_FLAG_HIDDEN);
        }
        stop_state_watch();
    }
    // CONNECTING / DISCONNECTED: keep waiting
}

/**
 * @brief Attempt WiFi connection with entered password
 */
static void attempt_connection(void)
{
    if (ui_password_ta == NULL || s_target_ssid[0] == '\0') {
        ESP_LOGW(TAG, "Cannot connect: no SSID or text area");
        return;
    }

    // Get the password from text area
    const char *password = lv_textarea_get_text(ui_password_ta);

    if (password == NULL || strlen(password) < 8) {
        ESP_LOGW(TAG, "Password too short (min 8 characters)");
        // Show error
        if (ui_error_label != NULL) {
            lv_label_set_text(ui_error_label, "Min 8 chars");
            lv_obj_clear_flag(ui_error_label, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    // Store the password
    strncpy(s_password, password, MAX_PASSWORD_LEN);
    s_password[MAX_PASSWORD_LEN] = '\0';

    ESP_LOGI(TAG, "Attempting connection to '%s' with password (len=%d)",
             s_target_ssid, (int)strlen(s_password));

    // Hide any prior error before we start
    if (ui_error_label != NULL) {
        lv_obj_add_flag(ui_error_label, LV_OBJ_FLAG_HIDDEN);
    }

    // Start connection
    if (wifi_connect(s_target_ssid, s_password)) {
        ESP_LOGI(TAG, "Connection initiated, watching state for result");
        // Start polling for CONNECTED / FAILED. The screen stays visible so
        // we can show a useful reason code if it fails.
        stop_state_watch();
        s_state_watch_timer = lv_timer_create(state_watch_timer_cb,
                                              STATE_WATCH_INTERVAL_MS, NULL);
    } else {
        ESP_LOGE(TAG, "Failed to initiate connection");
        if (ui_error_label != NULL) {
            lv_label_set_text(ui_error_label, "Connect failed");
            lv_obj_clear_flag(ui_error_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/**
 * @brief Connect button handler
 * @note Called from LVGL context - lock is already held by LVGL task
 */
static void connect_btn_event_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Connect button clicked");
    attempt_connection();
}

/**
 * @brief Cancel button handler
 * @note Called from LVGL context - lock is already held by LVGL task
 */
static void cancel_btn_event_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Cancel button clicked");
    wifi_password_hide_internal();
}

/**
 * @brief Internal show function - may be called from LVGL context
 * @note Called from LVGL context - lock is already held by LVGL task
 */
static void wifi_password_show_internal(void)
{
    // Ensure screen exists
    if (ui_Page_Password == NULL) {
        ESP_LOGI(TAG, "Password screen not created, creating now");
        ui_Page_Password_screen_init();
        if (ui_Page_Password == NULL) {
            ESP_LOGE(TAG, "Failed to create password screen - out of memory?");
            return;
        }
    }

    // Update SSID label
    if (ui_ssid_label != NULL) {
        char label_text[48];
        snprintf(label_text, sizeof(label_text), "Network: %s", s_target_ssid);
        lv_label_set_text(ui_ssid_label, label_text);
    }

    // Clear password field
    if (ui_password_ta != NULL) {
        lv_textarea_set_text(ui_password_ta, "");
    }

    // Hide error
    if (ui_error_label != NULL) {
        lv_obj_add_flag(ui_error_label, LV_OBJ_FLAG_HIDDEN);
    }

    // Reset show password checkbox
    if (ui_show_password_cb != NULL) {
        lv_obj_clear_state(ui_show_password_cb, LV_STATE_CHECKED);
        lv_textarea_set_password_mode(ui_password_ta, true);
    }

    // Check if we need to open the page
    lv_obj_t *current = lv_scr_act();
    if (current != ui_Page_Password) {
        ESP_LOGI(TAG, "Opening password page via lv_pm_open_page()");

        // Use page manager to show the screen
        lv_pm_open_page(g_main, &group_page_password, PM_ADD_OBJS_TO_GROUP,
                        &ui_Page_Password, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0,
                        &ui_Page_Password_screen_init);
    } else {
        ESP_LOGI(TAG, "Already on password screen");
    }
}

/**
 * @brief Internal hide function - may be called from LVGL context
 * @note Called from LVGL context - lock is already held by LVGL task
 */
static void wifi_password_hide_internal(void)
{
    // Stop the state-watch timer if it's still running (e.g. user cancels
    // mid-connect). Otherwise it could fire against a torn-down screen.
    stop_state_watch();

    // Clear password from text area before leaving (security)
    if (ui_password_ta != NULL) {
        lv_textarea_set_text(ui_password_ta, "");
    }

    // Clear password buffer (security - minimize exposure window)
    memset(s_password, 0, sizeof(s_password));

    // Use page manager to return to previous
    lv_pm_return_to_previous();
}

// ============================================================================
// Public API
// ============================================================================

void wifi_password_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi password module");

    // Create the password screen (within LVGL lock)
    if (lvgl_port_lock(100)) {
        ui_Page_Password_screen_init();
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "WiFi password module initialized");
}

void wifi_password_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing WiFi password module");

    if (!lvgl_port_lock(100)) {
        ESP_LOGW(TAG, "Could not acquire LVGL lock for deinit");
        return;
    }

    stop_state_watch();

    // Delete the screen
    if (ui_Page_Password != NULL) {
        lv_obj_del(ui_Page_Password);
        ui_Page_Password = NULL;
        ui_ssid_label = NULL;
        ui_password_ta = NULL;
        ui_show_password_cb = NULL;
        ui_keyboard = NULL;
        ui_connect_btn = NULL;
        ui_cancel_btn = NULL;
        ui_error_label = NULL;
    }

    // Clear stored data
    s_target_ssid[0] = '\0';
    memset(s_password, 0, sizeof(s_password));

    lvgl_port_unlock();
    ESP_LOGI(TAG, "WiFi password module deinitialized");
}

void wifi_password_show(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0') {
        ESP_LOGW(TAG, "Cannot show password screen: no SSID provided");
        return;
    }

    ESP_LOGI(TAG, "Showing password screen for SSID: '%s'", ssid);

    // Store the target SSID
    strncpy(s_target_ssid, ssid, sizeof(s_target_ssid) - 1);
    s_target_ssid[sizeof(s_target_ssid) - 1] = '\0';

    // Clear previous password
    memset(s_password, 0, sizeof(s_password));

    // Try to acquire LVGL lock
    bool acquired_lock = lvgl_port_lock(500);
    if (!acquired_lock) {
        ESP_LOGW(TAG, "Could not get LVGL lock for show");
        return;
    }

    wifi_password_show_internal();

    lvgl_port_unlock();
}

void wifi_password_hide(void)
{
    ESP_LOGI(TAG, "Hiding password screen");

    bool acquired_lock = lvgl_port_lock(100);
    if (!acquired_lock) {
        ESP_LOGW(TAG, "Could not get LVGL lock for hide");
        return;
    }

    wifi_password_hide_internal();

    lvgl_port_unlock();
}

bool wifi_password_is_visible(void)
{
    if (!lvgl_port_lock(50)) {
        return false;
    }

    bool visible = (ui_Page_Password != NULL) && (lv_scr_act() == ui_Page_Password);

    lvgl_port_unlock();
    return visible;
}

/**
 * @brief Get the entered password
 *
 * @return Pointer to the password string (empty if none entered)
 *
 * @note Thread safety: This function returns a pointer to internal static
 *       buffer that is modified by LVGL callbacks. The returned pointer
 *       is only valid while holding the LVGL lock or immediately after
 *       a connection attempt. Copy the value if needed outside LVGL context.
 *
 * @warning The password buffer is cleared when the screen is hidden for
 *          security. Only access this immediately after connection attempt.
 */
const char* wifi_password_get_password(void)
{
    return s_password;
}

void wifi_password_clear(void)
{
    memset(s_password, 0, sizeof(s_password));

    if (lvgl_port_lock(100)) {
        if (ui_password_ta != NULL) {
            lv_textarea_set_text(ui_password_ta, "");
        }
        if (ui_error_label != NULL) {
            lv_obj_add_flag(ui_error_label, LV_OBJ_FLAG_HIDDEN);
        }
        lvgl_port_unlock();
    }
}
