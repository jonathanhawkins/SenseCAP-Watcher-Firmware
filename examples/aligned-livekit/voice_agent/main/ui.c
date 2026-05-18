#include "ui.h"
#include "esp_lvgl_port.h"
#include "wifi_scan.h"
#include "wifi_setup.h"
#include "esp_log.h"
#include "example.h"

static const char *TAG = "ui";

// WiFi button (always visible — restyled based on connection state)
static lv_obj_t *wifi_btn = NULL;
static lv_obj_t *wifi_btn_label = NULL;
static lv_timer_t *wifi_check_timer = NULL;
#define WIFI_CHECK_INTERVAL_MS 2000

//=============================================================================
// Status Bar - WiFi icon + Voice chat indicator at top of screen
//=============================================================================
static lv_obj_t *status_bar = NULL;        // Container for status icons
static lv_obj_t *wifi_status_icon = NULL;  // WiFi icon label
static lv_obj_t *voice_status_icon = NULL; // Voice chat icon label
static lv_obj_t *hint_label = NULL;        // "Hold knob to disconnect" hint
static bool s_voice_active = false;        // Track voice chat state
static bool s_last_wifi_connected = false; // Track WiFi state for change detection

// Forward declarations
static void wifi_btn_event_cb(lv_event_t *e);
static void wifi_check_timer_cb(lv_timer_t *timer);
static void create_wifi_button(void);
static void update_wifi_button_visibility(void);
static void create_status_bar(void);
static void update_status_bar(void);
static void create_hint_label(void);
static void update_hint_label(void);

// Assume that the images have been converted to C arrays and included
extern const lv_img_dsc_t speaking_A;
extern const lv_img_dsc_t speaking_B;
extern const lv_img_dsc_t speaking_C;
extern const lv_img_dsc_t speaking_D;
extern const lv_img_dsc_t speaking_E;

extern const lv_img_dsc_t listening_A;
extern const lv_img_dsc_t listening_B;
extern const lv_img_dsc_t listening_C;
extern const lv_img_dsc_t listening_D;
extern const lv_img_dsc_t listening_E;

// Image arrays
static const lv_img_dsc_t *speaking_images[] = {
    &speaking_A,
    &speaking_B,
    &speaking_C,
    &speaking_D,
    &speaking_E
};

static const lv_img_dsc_t *listening_images[] = {
    &listening_A,
    &listening_B,
    &listening_C,
    &listening_D,
    &listening_E
};
static lv_obj_t *label;
static lv_obj_t *img; // Image object
static uint8_t current_image_index = 0; // Index of the currently displayed image
static bool is_speaking = false; // Whether the speaking images are currently displayed
static lv_timer_t *timer1 = NULL; // Timer 1 (switches to listening after 2.5s)
static lv_timer_t *timer2 = NULL; // Timer 2 (500ms image polling)

// Timer 2 callback function (500ms image polling)
static void timer2_callback(lv_timer_t *timer)
{
    const lv_img_dsc_t **images = is_speaking ? speaking_images : listening_images;
    current_image_index = (current_image_index + 1) % (sizeof(speaking_images) / sizeof(speaking_images[0]));
    lv_img_set_src(img, images[current_image_index]); // Update the image
}

// Timer 1 callback function (switches to listening after 2.5s)
static void timer1_callback(lv_timer_t *timer)
{
    is_speaking = false; // Switch to listening images
    lv_timer_reset(timer2); // Reset Timer 2
}

// Switch to speaking images
void ui_switch_speaking(void)
{
    lvgl_port_lock(0);
    if (!is_speaking) {
        // If not currently displaying speaking images, switch to speaking images
        is_speaking = true;
        current_image_index = 0;
        lv_img_set_src(img, speaking_images[current_image_index]); // Set the initial image

        // Start Timer 1 (switch to listening after 1s)
        if (timer1) {
            lv_timer_reset(timer1);
        } else {
            timer1 = lv_timer_create(timer1_callback, 1000, NULL); // 1s timer
        }

    } else {
        // If already displaying speaking images, reset Timer 1
        if (timer1) {
            lv_timer_reset(timer1);
        }
    }
    lvgl_port_unlock();
}

void ui_listening(void)
{
    lvgl_port_lock(0);
    img = lv_img_create(lv_scr_act());
    lv_img_set_src(img, listening_images[current_image_index]); // Set the initial image to listening
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0); // Center the image
    timer2 = lv_timer_create(timer2_callback, 300, NULL); // 300ms timer
    lv_timer_set_repeat_count(timer2, -1);

    // Show status bar and hint when entering listening/voice mode
    s_voice_active = true;
    create_status_bar();
    update_status_bar();
    create_hint_label();
    update_hint_label();

    lvgl_port_unlock();
}

void ui_wifi_connecting(void)
{
    lvgl_port_lock(0);
    if (label) {
        lv_label_set_text(label, "Wi-Fi Connecting...");
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    }
    lvgl_port_unlock();
}

void ui_init(void)
{
    lvgl_port_lock(0);
    label = lv_label_create(lv_scr_act()); // Create a label on the active screen
    lv_label_set_text(label, "Configure Wifi and OpenAI key via serial port.");

    // Set the label width to screen width (to enable scrolling)
    lv_obj_set_width(label, LV_PCT(100)); // 100% of parent width
    // Enable long mode for scrolling
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR); // Circular scroll

    // Align the label to the center of the screen
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0); // Center alignment with no offset
    lvgl_port_unlock();
}

void ui_disconnecting(void)
{
    lvgl_port_lock(0);
    // Stop any running animation timers
    if (timer1) {
        lv_timer_del(timer1);
        timer1 = NULL;
    }
    if (timer2) {
        lv_timer_del(timer2);
        timer2 = NULL;
    }
    // Mark voice as inactive
    s_voice_active = false;
    // Clean screen and show disconnecting message
    lv_obj_clean(lv_scr_act());
    // Reset status bar pointers since lv_obj_clean destroys all children
    status_bar = NULL;
    wifi_status_icon = NULL;
    voice_status_icon = NULL;
    hint_label = NULL;
    wifi_btn = NULL;
    wifi_btn_label = NULL;
    img = NULL;

    lv_obj_t *disconnect_label = lv_label_create(lv_scr_act());
    lv_label_set_text(disconnect_label, "Disconnecting...");
    lv_obj_set_style_text_font(disconnect_label, &lv_font_montserrat_14, 0);
    lv_obj_center(disconnect_label);
    lvgl_port_unlock();
}

void ui_powering_off(void)
{
    lvgl_port_lock(0);
    // Stop any running animation timers
    if (timer1) {
        lv_timer_del(timer1);
        timer1 = NULL;
    }
    if (timer2) {
        lv_timer_del(timer2);
        timer2 = NULL;
    }
    s_voice_active = false;
    // Clean screen and show power off message
    lv_obj_clean(lv_scr_act());
    // Reset pointers since lv_obj_clean destroys all children
    status_bar = NULL;
    wifi_status_icon = NULL;
    voice_status_icon = NULL;
    hint_label = NULL;
    wifi_btn = NULL;
    wifi_btn_label = NULL;
    img = NULL;

    lv_obj_t *poweroff_label = lv_label_create(lv_scr_act());
    lv_label_set_text(poweroff_label, "Goodbye!");
    lv_obj_set_style_text_font(poweroff_label, &lv_font_montserrat_14, 0);
    lv_obj_center(poweroff_label);
    lvgl_port_unlock();
}

//=============================================================================
// Status Bar - WiFi and Voice Chat indicators at top of screen
//=============================================================================

/**
 * @brief Create the status bar container with WiFi and voice icons
 *
 * The status bar sits at the top of the round screen with a semi-transparent
 * background. It shows:
 *   - WiFi icon (green when connected, red when disconnected)
 *   - Voice chat icon (green when active, grey when inactive)
 */
static void create_status_bar(void)
{
    if (status_bar != NULL) {
        return; // Already created
    }

    // Create container at top of screen
    status_bar = lv_obj_create(lv_scr_act());
    if (status_bar == NULL) {
        ESP_LOGE(TAG, "Failed to create status bar");
        return;
    }

    // Style: semi-transparent dark background, positioned at top
    // For a 412x412 round display, the usable width at top is narrower
    lv_obj_set_size(status_bar, 120, 30);
    lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 25); // Offset down from edge for round screen
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_60, 0);
    lv_obj_set_style_radius(status_bar, 15, 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_pad_all(status_bar, 4, 0);
    lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);

    // Use flex layout for horizontal icon arrangement
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // WiFi status icon
    wifi_status_icon = lv_label_create(status_bar);
    lv_label_set_text(wifi_status_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(wifi_status_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(wifi_status_icon, lv_color_hex(0x808080), 0); // Grey initially

    // Voice chat status icon (using audio symbol)
    voice_status_icon = lv_label_create(status_bar);
    lv_label_set_text(voice_status_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(voice_status_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(voice_status_icon, lv_color_hex(0x808080), 0); // Grey initially

    ESP_LOGI(TAG, "Status bar created");
}

/**
 * @brief Update status bar icon colors based on current state
 */
static void update_status_bar(void)
{
    if (status_bar == NULL) {
        return;
    }

    bool wifi_connected = wifi_is_connected();

    // Update WiFi icon color
    if (wifi_status_icon) {
        if (wifi_connected) {
            lv_obj_set_style_text_color(wifi_status_icon, lv_color_hex(0x4CAF50), 0); // Green
        } else {
            lv_obj_set_style_text_color(wifi_status_icon, lv_color_hex(0xF44336), 0); // Red
        }
    }

    // Update voice chat icon color
    if (voice_status_icon) {
        if (s_voice_active && room_is_active()) {
            lv_obj_set_style_text_color(voice_status_icon, lv_color_hex(0x4CAF50), 0); // Green
        } else {
            lv_obj_set_style_text_color(voice_status_icon, lv_color_hex(0x808080), 0); // Grey
        }
    }

    s_last_wifi_connected = wifi_connected;
}

//=============================================================================
// Hint Label - Shows "Hold knob to disconnect" during voice chat
//=============================================================================

/**
 * @brief Create the hint label at bottom of screen
 */
static void create_hint_label(void)
{
    if (hint_label != NULL) {
        return; // Already created
    }

    hint_label = lv_label_create(lv_scr_act());
    if (hint_label == NULL) {
        ESP_LOGE(TAG, "Failed to create hint label");
        return;
    }

    lv_label_set_text(hint_label, "Hold knob to disconnect");
    lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint_label, lv_color_hex(0xAAAAAA), 0); // Light grey
    lv_obj_set_style_text_align(hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, -35); // Above the round edge
    lv_obj_add_flag(hint_label, LV_OBJ_FLAG_HIDDEN); // Hidden by default

    ESP_LOGI(TAG, "Hint label created");
}

/**
 * @brief Show/hide hint label based on voice chat state
 */
static void update_hint_label(void)
{
    if (hint_label == NULL) {
        return;
    }

    if (s_voice_active && room_is_active()) {
        lv_obj_clear_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
    }
}

//=============================================================================
// Public API - Voice chat state management
//=============================================================================

void ui_set_voice_active(bool active)
{
    lvgl_port_lock(0);
    s_voice_active = active;
    update_status_bar();
    update_hint_label();
    lvgl_port_unlock();
}

//=============================================================================
// WiFi Button - Shows when WiFi is disconnected
//=============================================================================

/**
 * @brief WiFi button click event handler
 */
static void wifi_btn_event_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "WiFi button pressed, opening WiFi setup...");
    wifi_setup_init();
    wifi_setup_show();
}

/**
 * @brief Timer callback to check WiFi status and update button/status visibility
 */
static void wifi_check_timer_cb(lv_timer_t *timer)
{
    update_wifi_button_visibility();
    update_status_bar();
    update_hint_label();
}

/**
 * @brief Create the WiFi setup button at bottom center of screen
 */
static void create_wifi_button(void)
{
    if (wifi_btn != NULL) {
        return; // Already created
    }

    // Create button on active screen
    wifi_btn = lv_btn_create(lv_scr_act());
    if (wifi_btn == NULL) {
        ESP_LOGE(TAG, "Failed to create WiFi button");
        return;
    }

    // Position at bottom center (412x412 display, leave margin for round edges)
    // Y offset: positive moves down from center, ~150 pixels down
    lv_obj_set_size(wifi_btn, 140, 44);
    lv_obj_align(wifi_btn, LV_ALIGN_BOTTOM_MID, 0, -40);

    // Style: dark semi-transparent background
    lv_obj_set_style_bg_color(wifi_btn, lv_color_hex(0x2196F3), 0); // Blue
    lv_obj_set_style_bg_opa(wifi_btn, LV_OPA_90, 0);
    lv_obj_set_style_radius(wifi_btn, 22, 0);
    lv_obj_set_style_border_width(wifi_btn, 0, 0);
    lv_obj_set_style_shadow_width(wifi_btn, 8, 0);
    lv_obj_set_style_shadow_color(wifi_btn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(wifi_btn, LV_OPA_30, 0);

    // Pressed state
    lv_obj_set_style_bg_color(wifi_btn, lv_color_hex(0x1976D2), LV_STATE_PRESSED);

    // Add WiFi icon and text label (text/style updated in update_wifi_button_visibility)
    wifi_btn_label = lv_label_create(wifi_btn);
    lv_label_set_text(wifi_btn_label, LV_SYMBOL_WIFI " Setup");
    lv_obj_set_style_text_font(wifi_btn_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(wifi_btn_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(wifi_btn_label);

    // Add click event
    lv_obj_add_event_cb(wifi_btn, wifi_btn_event_cb, LV_EVENT_CLICKED, NULL);

    ESP_LOGI(TAG, "WiFi button created");
}

/**
 * @brief Update WiFi button style and label based on connection status
 *
 * The button is always visible so users can re-open WiFi setup (to change
 * networks, check status, etc.) from the home screen at any time. We just
 * restyle it: a prominent blue "Setup" CTA when disconnected, a small dark
 * "WiFi" chip when connected so it doesn't compete with the listening orb.
 */
static void update_wifi_button_visibility(void)
{
    if (wifi_btn == NULL || wifi_btn_label == NULL) {
        return;
    }

    bool connected = wifi_is_connected();

    // Ensure button stays visible regardless of state
    if (lv_obj_has_flag(wifi_btn, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(wifi_btn, LV_OBJ_FLAG_HIDDEN);
    }

    if (connected) {
        // Compact dark chip — don't compete with the listening orb
        lv_obj_set_size(wifi_btn, 110, 36);
        lv_obj_align(wifi_btn, LV_ALIGN_BOTTOM_MID, 0, -25);
        lv_obj_set_style_bg_color(wifi_btn, lv_color_hex(0x202020), 0);
        lv_obj_set_style_bg_opa(wifi_btn, LV_OPA_60, 0);
        lv_obj_set_style_radius(wifi_btn, 18, 0);
        lv_obj_set_style_shadow_opa(wifi_btn, LV_OPA_20, 0);
        lv_obj_set_style_bg_color(wifi_btn, lv_color_hex(0x303030), LV_STATE_PRESSED);
        lv_label_set_text(wifi_btn_label, LV_SYMBOL_WIFI " WiFi");
    } else {
        // Prominent blue CTA when no network configured
        lv_obj_set_size(wifi_btn, 140, 44);
        lv_obj_align(wifi_btn, LV_ALIGN_BOTTOM_MID, 0, -40);
        lv_obj_set_style_bg_color(wifi_btn, lv_color_hex(0x2196F3), 0);
        lv_obj_set_style_bg_opa(wifi_btn, LV_OPA_90, 0);
        lv_obj_set_style_radius(wifi_btn, 22, 0);
        lv_obj_set_style_shadow_opa(wifi_btn, LV_OPA_30, 0);
        lv_obj_set_style_bg_color(wifi_btn, lv_color_hex(0x1976D2), LV_STATE_PRESSED);
        lv_label_set_text(wifi_btn_label, LV_SYMBOL_WIFI " Setup");
    }
}

/**
 * @brief Initialize and show the WiFi button (call after ui_init or ui_listening)
 */
void ui_show_wifi_button(void)
{
    lvgl_port_lock(0);

    create_wifi_button();
    update_wifi_button_visibility();

    // Create status bar (always visible when WiFi button system is active)
    create_status_bar();
    update_status_bar();

    // Start timer to periodically check WiFi status
    if (wifi_check_timer == NULL) {
        wifi_check_timer = lv_timer_create(wifi_check_timer_cb, WIFI_CHECK_INTERVAL_MS, NULL);
        lv_timer_set_repeat_count(wifi_check_timer, -1); // Repeat forever
    }

    lvgl_port_unlock();
}

/**
 * @brief Hide and cleanup the WiFi button
 */
void ui_hide_wifi_button(void)
{
    lvgl_port_lock(0);

    if (wifi_check_timer) {
        lv_timer_del(wifi_check_timer);
        wifi_check_timer = NULL;
    }

    if (wifi_btn) {
        lv_obj_del(wifi_btn);
        wifi_btn = NULL;
        wifi_btn_label = NULL;  // Child of wifi_btn, freed by lv_obj_del
    }

    lvgl_port_unlock();
}
