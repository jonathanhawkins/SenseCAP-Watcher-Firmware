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

// Volume HUD widgets — created directly on the current screen (lv_scr_act())
// as a true widget overlay instead of a full screen swap. The home-screen orb
// (Aligned wave logo), status bar, hint label, and WiFi button all stay
// visible behind the semi-transparent chip.
static lv_obj_t *ui_volume_overlay = NULL;    // Semi-transparent chip container
static lv_obj_t *ui_volume_text    = NULL;    // "Volume  75%" label
static lv_obj_t *ui_vslider        = NULL;    // Visual slider bar

// Group info — kept for any consumers but no longer drives page navigation.
static GroupInfo group_page_volume;

// Timer for auto-hide
static lv_timer_t *s_hud_hide_timer = NULL;

// Encoder input device reference
static lv_indev_t *s_encoder_indev = NULL;

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
 * @brief Build the volume HUD chip as an overlay on the current home screen.
 *
 * Creates widgets directly on lv_scr_act() rather than swapping screens, so
 * the home-screen orb (Aligned wave logo), status bar, hint label, and WiFi
 * button remain visible behind the semi-transparent chip.
 */
static void ui_Page_Volume_screen_init(void)
{
    if (ui_volume_overlay != NULL) {
        return;  // Already created
    }

    ESP_LOGI(TAG, "Creating volume HUD overlay on current screen");

    // Single compact chip just above the WiFi button. ~280×84 leaves the orb
    // and status bar fully visible. Semi-transparent dark bg so the orb
    // shows through behind the chip.
    ui_volume_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(ui_volume_overlay, 280, 84);
    lv_obj_align(ui_volume_overlay, LV_ALIGN_BOTTOM_MID, 0, -90);
    lv_obj_clear_flag(ui_volume_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_volume_overlay, 18, 0);
    lv_obj_set_style_bg_color(ui_volume_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ui_volume_overlay, LV_OPA_70, 0);  // ~70% — orb shows through
    lv_obj_set_style_border_width(ui_volume_overlay, 0, 0);
    lv_obj_set_style_shadow_width(ui_volume_overlay, 8, 0);
    lv_obj_set_style_shadow_color(ui_volume_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(ui_volume_overlay, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(ui_volume_overlay, 12, 0);
    lv_obj_add_flag(ui_volume_overlay, LV_OBJ_FLAG_HIDDEN);  // Hidden until first volume change

    // "Volume  75%" text on top of the chip
    ui_volume_text = lv_label_create(ui_volume_overlay);
    lv_label_set_text(ui_volume_text, "Volume  0%");
    lv_obj_set_style_text_color(ui_volume_text, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(ui_volume_text, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(ui_volume_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ui_volume_text, LV_ALIGN_TOP_MID, 0, 0);

    // Thin slider below — green fill on dark track, white knob with green border.
    ui_vslider = lv_slider_create(ui_volume_overlay);
    lv_slider_set_range(ui_vslider, VOLUME_MIN, VOLUME_MAX);
    lv_slider_set_value(ui_vslider, s_current_volume, LV_ANIM_OFF);
    lv_obj_set_size(ui_vslider, 240, 10);
    lv_obj_align(ui_vslider, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_color(ui_vslider, lv_color_hex(0x404040), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_vslider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_vslider, lv_color_hex(0x8FC31F), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ui_vslider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui_vslider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(ui_vslider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_color(ui_vslider, lv_color_hex(0x8FC31F), LV_PART_KNOB);
    lv_obj_set_style_border_opa(ui_vslider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_width(ui_vslider, 3, LV_PART_KNOB);

    // Group info kept for compatibility with any consumers that read it.
    group_page_volume.obj_count = 1;
    group_page_volume.group[0] = ui_vslider;

    ESP_LOGI(TAG, "Volume overlay created: %p, slider: %p", ui_volume_overlay, ui_vslider);
}

/**
 * @brief Update volume HUD values
 */
static void volume_screen_update(void)
{
    if (ui_volume_text != NULL) {
        char buf[24];
        snprintf(buf, sizeof(buf), "Volume  %d%%", s_current_volume);
        lv_label_set_text(ui_volume_text, buf);
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

    // Don't pre-create the volume overlay here — at this point the home
    // screen isn't fully populated, and we'd attach the overlay to the wrong
    // parent. It's created lazily on the first show_hud call.

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

    if (ui_volume_overlay != NULL) {
        lv_obj_del(ui_volume_overlay);
        ui_volume_overlay = NULL;
        ui_volume_text = NULL;
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
 * @brief Show the volume HUD as an overlay on the current screen.
 *
 * The chip sits on top of whatever screen is active (home screen with orb,
 * status bar, WiFi button) — nothing gets hidden or swapped out. Auto-hides
 * after VOLUME_HUD_DISPLAY_MS.
 */
void volume_control_show_hud(void)
{
    ESP_LOGI(TAG, "show_hud called, volume=%d%%", s_current_volume);

    if (!lvgl_port_lock(500)) {
        ESP_LOGW(TAG, "Could not get LVGL lock for HUD show");
        return;
    }

    // Cancel existing hide timer (encoder rotation pushes the auto-hide out)
    if (s_hud_hide_timer != NULL) {
        lv_timer_del(s_hud_hide_timer);
        s_hud_hide_timer = NULL;
    }

    // Lazy-build the HUD on first use AND rebuild it if the user has navigated
    // to a different screen since the last show. The overlay is a child of
    // lv_scr_act() — if that has changed (e.g. user opened WiFi setup), the
    // existing chip lives on the wrong screen and would be invisible.
    lv_obj_t *current_screen = lv_scr_act();
    if (ui_volume_overlay != NULL && lv_obj_get_parent(ui_volume_overlay) != current_screen) {
        ESP_LOGI(TAG, "Screen changed; rebuilding volume overlay on current screen");
        lv_obj_del(ui_volume_overlay);
        ui_volume_overlay = NULL;
        ui_volume_text = NULL;
        ui_vslider = NULL;
    }
    if (ui_volume_overlay == NULL) {
        ESP_LOGI(TAG, "Volume overlay not created, building now");
        ui_Page_Volume_screen_init();
    }

    // Update volume display values
    volume_screen_update();

    // Show the chip on top of all chrome.
    if (ui_volume_overlay != NULL) {
        lv_obj_clear_flag(ui_volume_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(ui_volume_overlay);
    }

    // Schedule auto-hide.
    s_hud_hide_timer = lv_timer_create(hud_hide_timer_cb, VOLUME_HUD_DISPLAY_MS, NULL);
    lv_timer_set_repeat_count(s_hud_hide_timer, 1);

    lvgl_port_unlock();
}

/**
 * @brief Hide the volume HUD overlay.
 *
 * Just hides the chip widget — doesn't touch the underlying screen or nav stack.
 */
void volume_control_hide_hud(void)
{
    ESP_LOGI(TAG, "Hiding volume overlay");

    if (!lvgl_port_lock(100)) {
        ESP_LOGW(TAG, "Could not get LVGL lock for HUD hide");
        return;
    }

    if (ui_volume_overlay != NULL) {
        lv_obj_add_flag(ui_volume_overlay, LV_OBJ_FLAG_HIDDEN);
    }

    lvgl_port_unlock();
}
