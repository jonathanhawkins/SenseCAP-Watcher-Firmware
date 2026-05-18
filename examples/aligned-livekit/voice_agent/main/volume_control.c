/**
 * @file volume_control.c
 * @brief Volume control with rotary encoder wheel support and HUD overlay
 *
 * Uses the Page Manager approach for screen transitions.
 * Creates a SEPARATE knob handle for volume callbacks (independent from LVGL encoder)
 */

#include "volume_control.h"
#include "pm.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "board.h"

static const char *TAG = "volume_ctrl";

// Current volume level (0-100)
static int s_current_volume = VOLUME_DEFAULT;

// Debounce: minimum time between volume updates (100ms)
#define VOLUME_UPDATE_DEBOUNCE_US  100000

// Boot delay: ignore encoder events for first 5 seconds after init
#define BOOT_DELAY_US  5000000
static int64_t s_init_time = 0;
static int64_t s_last_update_time = 0;

// Knob GPIO pins (from sensecap-watcher.h)
#define VOLUME_KNOB_A   (GPIO_NUM_41)
#define VOLUME_KNOB_B   (GPIO_NUM_42)

// Volume screen objects - created exactly like factory firmware ui_Page_Slider.c
static lv_obj_t *ui_Page_Volume = NULL;   // The volume screen (lv_obj_create(NULL))
static lv_obj_t *ui_volume_container = NULL;  // Container for layout
static lv_obj_t *ui_volume_label = NULL;      // "Volume" text label
static lv_obj_t *ui_volume_value = NULL;      // Value label (e.g., "85")
static lv_obj_t *ui_volume_percent = NULL;    // "%" label
static lv_obj_t *ui_slider_container = NULL;  // Container for slider
static lv_obj_t *ui_vslider = NULL;           // Visual slider bar (named like factory)

// Group info for volume page objects
static GroupInfo group_page_volume;

// Timer for auto-hide
static lv_timer_t *s_hud_hide_timer = NULL;

// Encoder input device reference
static lv_indev_t *s_encoder_indev = NULL;

// Previous screen to return to
static lv_obj_t *s_previous_screen = NULL;

// Forward declarations
static void hud_hide_timer_cb(lv_timer_t *timer);
static void ui_Page_Volume_screen_init(void);
static void volume_screen_update(void);

/**
 * @brief Apply volume to the audio codec
 */
static esp_err_t apply_volume_to_codec(int volume)
{
    esp_codec_dev_handle_t play_handle = get_playback_handle();
    if (play_handle == NULL) {
        ESP_LOGW(TAG, "Playback handle not available");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_codec_dev_set_out_vol(play_handle, volume);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set volume: %s", esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief Create volume screen - EXACTLY like factory firmware ui_Page_Slider_screen_init()
 */
static void ui_Page_Volume_screen_init(void)
{
    if (ui_Page_Volume != NULL) {
        return;  // Already created
    }

    ESP_LOGI(TAG, "Creating volume screen (factory firmware pattern)");

    // Create screen exactly like factory: lv_obj_create(NULL)
    ui_Page_Volume = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_Page_Volume, LV_OBJ_FLAG_SCROLLABLE);
    // Use distinct dark gray background to make volume HUD clearly visible
    lv_obj_set_style_bg_color(ui_Page_Volume, lv_color_hex(0x303030), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Page_Volume, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    ESP_LOGI(TAG, "Volume screen background set to dark gray 0x303030");

    // Create container for volume info (like ui_bvpb in factory)
    ui_volume_container = lv_obj_create(ui_Page_Volume);
    lv_obj_set_width(ui_volume_container, 320);
    lv_obj_set_height(ui_volume_container, 67);
    lv_obj_set_x(ui_volume_container, 0);
    lv_obj_set_y(ui_volume_container, -100);
    lv_obj_set_align(ui_volume_container, LV_ALIGN_CENTER);
    lv_obj_set_flex_flow(ui_volume_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui_volume_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ui_volume_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_volume_container, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_volume_container, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_volume_container, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ui_volume_container, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    // "Volume" label (like ui_bvbt in factory)
    ui_volume_label = lv_label_create(ui_volume_container);
    lv_obj_set_width(ui_volume_label, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_volume_label, LV_SIZE_CONTENT);
    lv_obj_set_align(ui_volume_label, LV_ALIGN_CENTER);
    lv_label_set_text(ui_volume_label, "Volume ");
    lv_obj_set_style_text_color(ui_volume_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_volume_label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_volume_label, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Volume value label (like ui_bvbv in factory)
    ui_volume_value = lv_label_create(ui_volume_container);
    lv_obj_set_width(ui_volume_value, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_volume_value, LV_SIZE_CONTENT);
    lv_obj_set_align(ui_volume_value, LV_ALIGN_CENTER);
    lv_label_set_text(ui_volume_value, "0");
    lv_obj_set_style_text_color(ui_volume_value, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_volume_value, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_volume_value, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);

    // "%" label (like ui_bvs in factory)
    ui_volume_percent = lv_label_create(ui_volume_container);
    lv_obj_set_width(ui_volume_percent, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_volume_percent, LV_SIZE_CONTENT);
    lv_obj_set_align(ui_volume_percent, LV_ALIGN_CENTER);
    lv_label_set_text(ui_volume_percent, "%");
    lv_obj_set_style_text_color(ui_volume_percent, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_volume_percent, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_volume_percent, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Slider container (like ui_vp in factory)
    ui_slider_container = lv_obj_create(ui_Page_Volume);
    lv_obj_set_width(ui_slider_container, 380);
    lv_obj_set_height(ui_slider_container, 100);
    lv_obj_set_align(ui_slider_container, LV_ALIGN_CENTER);
    lv_obj_clear_flag(ui_slider_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_slider_container, 40, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_slider_container, lv_color_hex(0x202124), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_slider_container, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_slider_container, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ui_slider_container, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Volume slider (like ui_vslider in factory)
    ui_vslider = lv_slider_create(ui_slider_container);
    lv_slider_set_range(ui_vslider, VOLUME_MIN, VOLUME_MAX);
    lv_slider_set_value(ui_vslider, s_current_volume, LV_ANIM_OFF);
    lv_obj_set_width(ui_vslider, 250);
    lv_obj_set_height(ui_vslider, 30);
    lv_obj_set_align(ui_vslider, LV_ALIGN_CENTER);
    lv_obj_set_style_bg_color(ui_vslider, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_vslider, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Slider indicator (green like factory: 0x8FC31F)
    lv_obj_set_style_bg_color(ui_vslider, lv_color_hex(0x8FC31F), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_vslider, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    // Slider knob
    lv_obj_set_style_bg_color(ui_vslider, lv_color_hex(0xFFFFFF), LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_vslider, 255, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_vslider, lv_color_hex(0x8FC31F), LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ui_vslider, 255, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_vslider, 4, LV_PART_KNOB | LV_STATE_DEFAULT);

    // Setup group info for this page (like factory initGroup())
    group_page_volume.obj_count = 1;
    group_page_volume.group[0] = ui_vslider;

    ESP_LOGI(TAG, "Volume screen created: %p, slider: %p", ui_Page_Volume, ui_vslider);
}

/**
 * @brief Update volume screen values
 */
static void volume_screen_update(void)
{
    if (ui_volume_value != NULL) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", s_current_volume);
        lv_label_set_text(ui_volume_value, buf);
    }

    if (ui_vslider != NULL) {
        lv_slider_set_value(ui_vslider, s_current_volume, LV_ANIM_OFF);
    }
}

/**
 * @brief Timer callback to hide the HUD
 */
static void hud_hide_timer_cb(lv_timer_t *timer)
{
    volume_control_hide_hud();
    s_hud_hide_timer = NULL;
}

/**
 * @brief Check if volume updates are allowed (boot delay + debounce)
 */
static bool can_update_volume(void)
{
    int64_t now = esp_timer_get_time();

    // Ignore encoder events during boot (first 5 seconds)
    if ((now - s_init_time) < BOOT_DELAY_US) {
        ESP_LOGD(TAG, "Ignoring encoder during boot delay");
        return false;
    }

    // Debounce - 100ms between updates
    if ((now - s_last_update_time) < VOLUME_UPDATE_DEBOUNCE_US) {
        return false;
    }
    s_last_update_time = now;
    return true;
}

// ============================================================================
// Encoder polling for volume control
// ============================================================================

static lv_timer_t *s_encoder_poll_timer = NULL;
static uint8_t s_last_a_level = 1;
static uint8_t s_last_b_level = 1;
static int16_t s_encoder_count = 0;  // Accumulated encoder ticks
static int64_t s_last_edge_time = 0;  // For noise filtering

// Trigger on each edge for responsive feel (matches factory firmware)
#define ENCODER_TRIGGER_THRESHOLD  1
// Reset count if no edge for 300ms (user stopped turning)
#define ENCODER_TIMEOUT_US  300000

static void encoder_poll_timer_cb(lv_timer_t *timer)
{
    // Read current GPIO levels
    uint8_t a_level = gpio_get_level(VOLUME_KNOB_A);
    uint8_t b_level = gpio_get_level(VOLUME_KNOB_B);
    int64_t now = esp_timer_get_time();

    // Reset count if too much time has passed (user stopped turning)
    if ((now - s_last_edge_time) > ENCODER_TIMEOUT_US && s_encoder_count != 0) {
        s_encoder_count = 0;
    }

    // Detect A channel edge (both rising and falling for better resolution)
    if (a_level != s_last_a_level) {
        s_last_edge_time = now;

        // Determine direction based on A edge and B level
        // A falling + B high = CW, A falling + B low = CCW
        // A rising + B low = CW, A rising + B high = CCW
        int direction = 0;
        if (a_level == 0) {
            // A fell
            direction = (b_level == 1) ? 1 : -1;
        } else {
            // A rose
            direction = (b_level == 0) ? 1 : -1;
        }

        // Only accumulate if direction matches or count is zero
        if (s_encoder_count == 0 || (s_encoder_count > 0 && direction > 0) || (s_encoder_count < 0 && direction < 0)) {
            s_encoder_count += direction;
        } else {
            // Direction changed - reset
            s_encoder_count = direction;
        }

        // Debug: log edge detection
        ESP_LOGD(TAG, "Edge: A=%d B=%d dir=%d count=%d", a_level, b_level, direction, s_encoder_count);

        // Check if we've accumulated enough edges
        if (s_encoder_count >= ENCODER_TRIGGER_THRESHOLD) {
            s_encoder_count = 0;
            ESP_LOGI(TAG, ">>> Encoder CW rotation detected");
            if (can_update_volume()) {
                volume_control_down();
            }
        } else if (s_encoder_count <= -ENCODER_TRIGGER_THRESHOLD) {
            s_encoder_count = 0;
            ESP_LOGI(TAG, ">>> Encoder CCW rotation detected");
            if (can_update_volume()) {
                volume_control_up();
            }
        }

        s_last_a_level = a_level;
    }

    // Track B level for next comparison
    if (b_level != s_last_b_level) {
        s_last_b_level = b_level;
    }
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t volume_control_init(lv_indev_t *encoder)
{
    ESP_LOGI(TAG, "Initializing volume control (default: %d%%)", VOLUME_DEFAULT);

    s_init_time = esp_timer_get_time();
    s_encoder_indev = encoder;
    s_current_volume = VOLUME_DEFAULT;
    if (s_current_volume > VOLUME_MAX) {
        s_current_volume = VOLUME_MAX;
    }

    // Configure GPIO pins for encoder input
    gpio_config_t encoder_gpio_cfg = {
        .pin_bit_mask = (1ULL << VOLUME_KNOB_A) | (1ULL << VOLUME_KNOB_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&encoder_gpio_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure encoder GPIOs: %s", esp_err_to_name(ret));
        return ret;
    }

    // Read initial GPIO states
    s_last_a_level = gpio_get_level(VOLUME_KNOB_A);
    s_last_b_level = gpio_get_level(VOLUME_KNOB_B);
    ESP_LOGI(TAG, "Encoder initial state: A=%d B=%d", s_last_a_level, s_last_b_level);

    // Apply initial volume to codec
    ret = apply_volume_to_codec(s_current_volume);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not set initial volume (codec may not be ready yet)");
    }

    // Initialize Page Manager with encoder (must be done within LVGL lock)
    lvgl_port_lock(0);
    lv_pm_init(encoder);

    // Create the volume screen
    ui_Page_Volume_screen_init();

    // Create a timer to poll encoder state for volume control
    s_encoder_poll_timer = lv_timer_create(encoder_poll_timer_cb, 10, NULL);  // Poll every 10ms for responsiveness
    ESP_LOGI(TAG, "Encoder poll timer created (10ms interval)");

    lvgl_port_unlock();

    ESP_LOGI(TAG, "Volume control initialized successfully");
    return ESP_OK;
}

void volume_control_deinit(void)
{
    if (!lvgl_port_lock(100)) {
        ESP_LOGW(TAG, "Could not acquire LVGL lock for deinit");
        return;
    }

    // Delete encoder poll timer
    if (s_encoder_poll_timer != NULL) {
        lv_timer_del(s_encoder_poll_timer);
        s_encoder_poll_timer = NULL;
    }

    if (s_hud_hide_timer != NULL) {
        lv_timer_del(s_hud_hide_timer);
        s_hud_hide_timer = NULL;
    }

    if (ui_Page_Volume != NULL) {
        lv_obj_del(ui_Page_Volume);
        ui_Page_Volume = NULL;
        ui_volume_container = NULL;
        ui_volume_label = NULL;
        ui_volume_value = NULL;
        ui_volume_percent = NULL;
        ui_slider_container = NULL;
        ui_vslider = NULL;
    }

    lvgl_port_unlock();
    ESP_LOGI(TAG, "Volume control deinitialized");
}

int volume_control_get(void)
{
    return s_current_volume;
}

esp_err_t volume_control_set(int volume)
{
    if (volume < VOLUME_MIN) {
        volume = VOLUME_MIN;
    } else if (volume > VOLUME_MAX) {
        volume = VOLUME_MAX;
    }

    if (volume == s_current_volume) {
        return ESP_OK;
    }

    s_current_volume = volume;
    ESP_LOGI(TAG, "Volume set to %d%%", s_current_volume);

    esp_err_t ret = apply_volume_to_codec(s_current_volume);

    // Update screen values if visible
    if (lvgl_port_lock(50)) {
        volume_screen_update();
        lvgl_port_unlock();
    }

    return ret;
}

void volume_control_up(void)
{
    int new_volume = s_current_volume + VOLUME_STEP;
    if (new_volume > VOLUME_MAX) {
        new_volume = VOLUME_MAX;
    }

    if (new_volume != s_current_volume) {
        volume_control_set(new_volume);
    }

    volume_control_show_hud();
}

void volume_control_down(void)
{
    int new_volume = s_current_volume - VOLUME_STEP;
    if (new_volume < VOLUME_MIN) {
        new_volume = VOLUME_MIN;
    }

    if (new_volume != s_current_volume) {
        volume_control_set(new_volume);
    }

    volume_control_show_hud();
}

/**
 * @brief Show the volume HUD
 *
 * Uses overlay pattern - does NOT affect the navigation stack.
 * This allows volume to be adjusted while in WiFi setup without corrupting navigation.
 */
void volume_control_show_hud(void)
{
    ESP_LOGI(TAG, "show_hud called, volume=%d%%", s_current_volume);

    if (!lvgl_port_lock(500)) {
        ESP_LOGW(TAG, "Could not get LVGL lock for HUD show");
        return;
    }

    // Cancel existing hide timer
    if (s_hud_hide_timer != NULL) {
        lv_timer_del(s_hud_hide_timer);
        s_hud_hide_timer = NULL;
    }

    // Make sure volume screen exists
    if (ui_Page_Volume == NULL) {
        ESP_LOGI(TAG, "Volume screen not created, creating now");
        ui_Page_Volume_screen_init();
    }

    // Update volume display values
    volume_screen_update();

    // Check if we're already on the volume screen
    lv_obj_t *current = lv_scr_act();
    if (current != ui_Page_Volume) {
        s_previous_screen = current;
        ESP_LOGI(TAG, "Opening volume page as OVERLAY (won't affect nav stack)");

        // Use OVERLAY pattern - doesn't push to navigation stack
        // This way volume adjustments won't corrupt the WiFi setup navigation
        lv_pm_open_overlay(&ui_Page_Volume, &ui_Page_Volume_screen_init);
    } else {
        ESP_LOGI(TAG, "Already on volume screen, just updating values");
    }

    // Create hide timer
    s_hud_hide_timer = lv_timer_create(hud_hide_timer_cb, VOLUME_HUD_DISPLAY_MS, NULL);
    lv_timer_set_repeat_count(s_hud_hide_timer, 1);

    lvgl_port_unlock();
}

/**
 * @brief Hide the volume HUD and return to previous screen
 *
 * Uses overlay close pattern - returns to the page that was showing before overlay.
 */
void volume_control_hide_hud(void)
{
    ESP_LOGI(TAG, "Hiding volume overlay, returning to previous screen");

    if (!lvgl_port_lock(100)) {
        ESP_LOGW(TAG, "Could not get LVGL lock for HUD hide");
        return;
    }

    // Use overlay close pattern - doesn't affect navigation stack
    lv_pm_close_overlay();

    lvgl_port_unlock();
}
