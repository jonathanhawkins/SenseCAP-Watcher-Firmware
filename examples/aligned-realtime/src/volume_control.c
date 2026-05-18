/**
 * @file volume_control.c
 * @brief Volume control with rotary encoder wheel support and HUD overlay
 *
 * Uses the EXACT same Page Manager approach as factory firmware (pm.c):
 * - lv_pm_init() for initialization
 * - lv_pm_open_page() for screen switching
 * - GroupInfo for encoder focus management
 *
 * Creates a SEPARATE knob handle for volume callbacks (independent from LVGL encoder)
 */

#include "volume_control.h"
#include "pm.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "iot_knob.h"
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
 *
 * Factory firmware pattern from ui_Page_Slider.c:
 * - ui_Page_Slider = lv_obj_create(NULL);
 * - lv_obj_clear_flag(ui_Page_Slider, LV_OBJ_FLAG_SCROLLABLE);
 * - lv_obj_set_style_bg_color(..., lv_color_hex(0x000000), ...);
 * - lv_obj_set_style_bg_opa(..., 255, ...);
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
    // Use dark blue background (0x1a1a2e) instead of pure black to verify screen switch works
    lv_obj_set_style_bg_color(ui_Page_Volume, lv_color_hex(0x1a1a2e), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Page_Volume, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    ESP_LOGI(TAG, "Volume screen background set to dark blue 0x1a1a2e");

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
    lv_obj_set_style_text_font(ui_volume_label, &lv_font_montserrat_32, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Volume value label (like ui_bvbv in factory)
    ui_volume_value = lv_label_create(ui_volume_container);
    lv_obj_set_width(ui_volume_value, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_volume_value, LV_SIZE_CONTENT);
    lv_obj_set_align(ui_volume_value, LV_ALIGN_CENTER);
    lv_label_set_text(ui_volume_value, "0");
    lv_obj_set_style_text_color(ui_volume_value, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_volume_value, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_volume_value, &lv_font_montserrat_32, LV_PART_MAIN | LV_STATE_DEFAULT);

    // "%" label (like ui_bvs in factory)
    ui_volume_percent = lv_label_create(ui_volume_container);
    lv_obj_set_width(ui_volume_percent, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_volume_percent, LV_SIZE_CONTENT);
    lv_obj_set_align(ui_volume_percent, LV_ALIGN_CENTER);
    lv_label_set_text(ui_volume_percent, "%");
    lv_obj_set_style_text_color(ui_volume_percent, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_volume_percent, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_volume_percent, &lv_font_montserrat_32, LV_PART_MAIN | LV_STATE_DEFAULT);

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
    lv_obj_t *volume_objects[] = {ui_vslider};
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
// Public API
// ============================================================================

/**
 * @brief Knob rotation callbacks - registered with esp_lvgl_port
 *
 * Uses lvgl_port_encoder_register_event_cb() to hook into the BSP's knob.
 */

static void knob_left_cb(void *arg, void *data)
{
    ESP_LOGI(TAG, ">>> Knob LEFT (CCW) - volume UP");
    if (can_update_volume()) {
        volume_control_up();
    }
}

static void knob_right_cb(void *arg, void *data)
{
    ESP_LOGI(TAG, ">>> Knob RIGHT (CW) - volume DOWN");
    if (can_update_volume()) {
        volume_control_down();
    }
}

esp_err_t volume_control_init(lv_indev_t *encoder)
{
    ESP_LOGI(TAG, "Initializing volume control (default: %d%%)", VOLUME_DEFAULT);

    s_init_time = esp_timer_get_time();
    s_encoder_indev = encoder;
    s_current_volume = VOLUME_DEFAULT;
    if (s_current_volume > VOLUME_MAX) {
        s_current_volume = VOLUME_MAX;
    }

    // Apply initial volume to codec
    esp_err_t ret = apply_volume_to_codec(s_current_volume);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not set initial volume (codec may not be ready yet)");
    }

    // Initialize Page Manager with encoder (must be done within LVGL lock)
    lvgl_port_lock(0);
    lv_pm_init(encoder);

    // Create the volume screen
    ui_Page_Volume_screen_init();

    // Register knob rotation callbacks with esp_lvgl_port
    if (encoder != NULL) {
        esp_err_t ret;
        ret = lvgl_port_encoder_register_event_cb(encoder, KNOB_LEFT, knob_left_cb, NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register KNOB_LEFT callback: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "KNOB_LEFT callback registered");
        }

        ret = lvgl_port_encoder_register_event_cb(encoder, KNOB_RIGHT, knob_right_cb, NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register KNOB_RIGHT callback: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "KNOB_RIGHT callback registered");
        }
    } else {
        ESP_LOGW(TAG, "No encoder provided, volume control via wheel disabled");
    }

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
 * Uses the EXACT same approach as factory firmware pm.c:
 * lv_pm_open_page(g_main, &group_page_volume, PM_ADD_OBJS_TO_GROUP, &ui_Page_Volume, ...)
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

    // Save current screen
    lv_obj_t *current = lv_scr_act();
    if (current != ui_Page_Volume) {
        s_previous_screen = current;
        ESP_LOGI(TAG, "Opening volume page via lv_pm_open_page()");

        // Use factory firmware pattern: lv_pm_open_page()
        // Use NONE animation first to test basic screen switch
        lv_pm_open_page(g_main, &group_page_volume, PM_ADD_OBJS_TO_GROUP,
                        &ui_Page_Volume, LV_SCR_LOAD_ANIM_NONE, 0, 0,
                        &ui_Page_Volume_screen_init);
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
 */
void volume_control_hide_hud(void)
{
    ESP_LOGI(TAG, "Hiding volume screen, returning to previous");

    if (!lvgl_port_lock(100)) {
        ESP_LOGW(TAG, "Could not get LVGL lock for HUD hide");
        return;
    }

    // Use page manager to return to previous
    lv_pm_return_to_previous();

    lvgl_port_unlock();
}
