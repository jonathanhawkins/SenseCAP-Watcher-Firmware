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
static lv_obj_t *hint_label = NULL;        // "Hold knob to disconnect" hint (under status bar)
static lv_obj_t *failure_label = NULL;     // "Auth failed — re-pair" (centered, red)
static lv_obj_t *failure_hint  = NULL;     // "Press knob to retry"  (under failure_label, grey)
static bool s_voice_active = false;        // Track voice chat state
static bool s_last_wifi_connected = false; // Track WiFi state for change detection
static bool s_disconnecting = false;       // Mid-disconnect — keep "Disconnecting..." hint up
static bool s_disconnecting_anim_active = false;
static lv_anim_t s_disconnecting_anim;

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

// Opacity-pulse callback for the "Disconnecting..." hint. We use opacity
// changes (not spatial movement) per watcher-ui.md — the partial-buffer
// renderer tears any thin geometry that translates across strips.
static void disconnecting_label_opa_cb(void *var, int32_t val)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)val, LV_PART_MAIN);
}

static void stop_disconnecting_anim(void)
{
    if (s_disconnecting_anim_active && hint_label) {
        lv_anim_del(hint_label, NULL);
        lv_obj_set_style_opa(hint_label, LV_OPA_COVER, LV_PART_MAIN);
    }
    s_disconnecting_anim_active = false;
}

// Timer 2 callback function (500ms image polling)
static void timer2_callback(lv_timer_t *timer)
{
    // Defensive: a leaked timer can outlive its img widget if cleanup paths
    // don't pair `lv_timer_del(timer2)` with `lv_obj_del(img)`. Without this
    // guard, lv_img_set_src(NULL, ...) crashes (null deref). See bug history
    // in the ui_listening() comment.
    if (img == NULL) return;
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
        if (img) lv_img_set_src(img, speaking_images[current_image_index]); // initial image

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

    // Reuse existing widget/timer instead of overwriting the globals. The
    // original code created a fresh img + timer2 on every call, leaking the
    // previous pair. The leaked timer kept ticking after lv_obj_clean() in
    // ui_disconnecting() wiped its associated img, so the next 300 ms fire
    // hit `lv_img_set_src(NULL, ...)` and panicked. ui_listening is called
    // from board.c at boot AND from on_state_changed(CONNECTED), so this
    // path is hot.
    if (img == NULL) {
        img = lv_img_create(lv_scr_act());
    }
    if (img) {
        lv_img_set_src(img, listening_images[current_image_index]); // initial image
        lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);                   // center it
    }
    if (timer2 == NULL) {
        timer2 = lv_timer_create(timer2_callback, 300, NULL);
        if (timer2) lv_timer_set_repeat_count(timer2, -1);
    }

    // Status bar + hint are owned by ui_set_voice_active(true), which the
    // caller invokes immediately after ui_listening() on the CONNECTED
    // state. Just ensure the widgets exist so that call has something to
    // update.
    //
    // DO NOT set `s_voice_active = true` here. ui_listening() is ALSO
    // called from board.c at boot to install the orb as the home wallpaper
    // — there's no session yet at that point. Setting s_voice_active=true
    // at boot makes the very first knob press flash "Hold to disconnect..."
    // via ui_knob_hold_start() (which gates on s_voice_active) before the
    // join-room flow can show "Connecting...". The CONNECTED path calls
    // ui_set_voice_active(true) explicitly, which is the only path that
    // should flip the flag true. The status bar's voice icon already AND's
    // `s_voice_active && room_is_active()` so it stays grey at boot
    // regardless of this flag.
    create_status_bar();
    update_status_bar();
    create_hint_label();

    // The orb is the most-recently-added child of the screen, so it sits at the
    // FRONT of the Z-order. The status bar / hint / WiFi button were created
    // earlier and would otherwise be hidden beneath the orb. Push them to the
    // front, and shove the orb to the back, so the chrome stays visible.
    lv_obj_move_background(img);
    if (status_bar) lv_obj_move_foreground(status_bar);
    if (hint_label) lv_obj_move_foreground(hint_label);
    if (wifi_btn)   lv_obj_move_foreground(wifi_btn);
    // Hide the boot config-scroll label if it's still around.
    if (label)         lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    // Clear any prior failure overlay — we connected.
    if (failure_label) lv_obj_add_flag(failure_label, LV_OBJ_FLAG_HIDDEN);
    if (failure_hint)  lv_obj_add_flag(failure_hint,  LV_OBJ_FLAG_HIDDEN);

    lvgl_port_unlock();
}

void ui_wifi_connecting(void)
{
    lvgl_port_lock(0);
    // Clear any prior failure overlay — the user is retrying.
    if (failure_label) lv_obj_add_flag(failure_label, LV_OBJ_FLAG_HIDDEN);
    if (failure_hint)  lv_obj_add_flag(failure_hint,  LV_OBJ_FLAG_HIDDEN);
    // Hide the boot scroll label, but KEEP the listening orb visible — it is
    // the Aligned-logo background of the home screen. The user wants only the
    // boot text gone, not the wallpaper.
    if (label) lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);

    // Show "Connecting..." in the same slot the "Hold knob to disconnect"
    // hint uses — TOP_MID, 0, 60 — so it sits cleanly under the status bar.
    create_hint_label();
    if (hint_label) {
        lv_label_set_text(hint_label, "Connecting...");
        lv_obj_set_style_text_color(hint_label, lv_color_hex(0xFFFFFF), 0); // bright white
        lv_obj_clear_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(hint_label);
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
    s_disconnecting = true;
    s_voice_active = false;

    // Stop the speaking→listening transition timer; the listening orb's image
    // poll (timer2) keeps running so the Aligned wallpaper stays animated.
    if (timer1) {
        lv_timer_del(timer1);
        timer1 = NULL;
    }
    is_speaking = false;

    // Keep all chrome alive — orb, status bar, WiFi button. Only swap the
    // hint widget to "Disconnecting..." with an opacity-pulse animation so
    // the user sees something is happening without spatial geometry that
    // would tear under the partial-buffer renderer (see watcher-ui.md).
    if (failure_label) lv_obj_add_flag(failure_label, LV_OBJ_FLAG_HIDDEN);
    if (failure_hint)  lv_obj_add_flag(failure_hint,  LV_OBJ_FLAG_HIDDEN);
    if (label)         lv_obj_add_flag(label,         LV_OBJ_FLAG_HIDDEN);

    create_hint_label();
    if (hint_label) {
        lv_label_set_text(hint_label, "Disconnecting...");
        lv_obj_set_style_text_color(hint_label, lv_color_hex(0xAAAAAA), 0); // light grey
        lv_obj_clear_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(hint_label);

        if (!s_disconnecting_anim_active) {
            lv_anim_init(&s_disconnecting_anim);
            lv_anim_set_var(&s_disconnecting_anim, hint_label);
            lv_anim_set_exec_cb(&s_disconnecting_anim, (lv_anim_exec_xcb_t)disconnecting_label_opa_cb);
            lv_anim_set_values(&s_disconnecting_anim, LV_OPA_60, LV_OPA_COVER);
            lv_anim_set_time(&s_disconnecting_anim, 800);
            lv_anim_set_playback_time(&s_disconnecting_anim, 800);
            lv_anim_set_repeat_count(&s_disconnecting_anim, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&s_disconnecting_anim);
            s_disconnecting_anim_active = true;
        }
    }

    update_status_bar(); // voice icon goes grey
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
    label = NULL;
    failure_label = NULL;
    failure_hint = NULL;

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

    // Voice-session status icon. LV_SYMBOL_CALL (phone receiver) reads
    // unambiguously as "live voice call" — better than LV_SYMBOL_AUDIO
    // (music notes) which suggested media playback. Grey when idle, green
    // when `s_voice_active && room_is_active()` — see update_status_bar().
    voice_status_icon = lv_label_create(status_bar);
    lv_label_set_text(voice_status_icon, LV_SYMBOL_CALL);
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
 * @brief Create the hint label below the top status bar
 *
 * Sits just under the status bar (which is at y=25, height=30 → bottom y=55).
 * This used to live at the bottom but the WiFi button now occupies that area
 * — the hint would have been hidden or overlapped on the home screen.
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
    lv_obj_align(hint_label, LV_ALIGN_TOP_MID, 0, 60); // Just under the status bar
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

    // Directly own the hint widget here rather than going through
    // update_hint_label(). Going through that helper means the 2-second
    // wifi_check_timer can re-hide the hint between transient states
    // (e.g. during CONNECTING when s_voice_active is still false).
    if (hint_label) {
        if (active) {
            // Connected — restore the "Hold knob to disconnect" hint and
            // cancel any mid-disconnect animation (re-connect supersedes).
            s_disconnecting = false;
            stop_disconnecting_anim();
            lv_label_set_text(hint_label, "Hold knob to disconnect");
            lv_obj_set_style_text_color(hint_label, lv_color_hex(0xAAAAAA), 0);
            lv_obj_clear_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(hint_label);
        } else if (!s_disconnecting) {
            // Voice off and not mid-disconnect — hide hint.
            lv_obj_add_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
        }
        // If s_disconnecting is true, leave the "Disconnecting..." hint up;
        // ui_show_wifi_button() / ui_listening() will clear it.
    }
    lvgl_port_unlock();
}

//=============================================================================
// Connection-failure overlay
//=============================================================================

void ui_connection_failed(const char *reason_text)
{
    if (reason_text == NULL || reason_text[0] == '\0') {
        reason_text = "Connection failed";
    }
    ESP_LOGW(TAG, "Showing connection failure: %s", reason_text);

    lvgl_port_lock(0);

    // Hide the boot scroll label. Keep the listening orb visible — it is the
    // home-screen wallpaper, and the red error label sits on top of it.
    if (label) lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    // Also hide the per-state hint ("Connecting...") so it doesn't overlap
    // the failure message.
    if (hint_label) lv_obj_add_flag(hint_label, LV_OBJ_FLAG_HIDDEN);

    // Failure label — sits in the same TOP_MID slot as `hint_label`
    // ("Connecting…" / "Hold knob to disconnect"), just under the status
    // bar. Previously this was LV_ALIGN_CENTER which overlapped the
    // wallpaper orb in the middle of the screen, making the message hard
    // to read against the wave logo. Now it shares the standard status
    // text slot so the user reads it the same way regardless of state.
    if (!failure_label) {
        failure_label = lv_label_create(lv_scr_act());
        lv_obj_set_style_text_font(failure_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(failure_label, lv_color_hex(0xF44336), 0); // red
        lv_obj_set_style_text_align(failure_label, LV_TEXT_ALIGN_CENTER, 0);
        // Constrain width so long messages wrap instead of running off the round edge.
        lv_obj_set_width(failure_label, 260);
        lv_label_set_long_mode(failure_label, LV_LABEL_LONG_WRAP);
    }
    lv_label_set_text(failure_label, reason_text);
    lv_obj_align(failure_label, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_clear_flag(failure_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(failure_label);

    // Retry hint anchored to the bottom of the failure_label so it
    // tracks dynamic wrap height — if the failure message is one line,
    // the hint sits ~8 px below; if it wraps to two lines, the hint
    // slides down with it instead of overlapping.
    if (!failure_hint) {
        failure_hint = lv_label_create(lv_scr_act());
        lv_obj_set_style_text_font(failure_hint, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(failure_hint, lv_color_hex(0xAAAAAA), 0); // grey
        lv_obj_set_style_text_align(failure_hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(failure_hint, "Press knob to retry");
    }
    lv_obj_align_to(failure_hint, failure_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
    lv_obj_clear_flag(failure_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(failure_hint);

    // Keep the chrome above the failure overlay so the WiFi button and status
    // bar remain reachable.
    if (status_bar) lv_obj_move_foreground(status_bar);
    if (wifi_btn)   lv_obj_move_foreground(wifi_btn);

    lvgl_port_unlock();
}

void ui_clear_connection_failure(void)
{
    lvgl_port_lock(0);
    if (failure_label) lv_obj_add_flag(failure_label, LV_OBJ_FLAG_HIDDEN);
    if (failure_hint)  lv_obj_add_flag(failure_hint,  LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

//=============================================================================
// Knob hold feedback (during active voice session)
//=============================================================================

void ui_knob_hold_start(void)
{
    // Outside an active room there's no hint to update — skip BEFORE
    // taking the LVGL lock. Reasons we gate on `room_is_active()` as well
    // as `s_voice_active`:
    //  * room_is_active() is the authoritative session-state check (it's
    //    held in example.c alongside the room handle). s_voice_active is
    //    only the UI's idea of session state.
    //  * Defense-in-depth: if a future change resurrects the boot-time
    //    s_voice_active=true (or any other path lets it drift), this gate
    //    still prevents the "Hold to disconnect..." flash on the very
    //    first knob press from the idle home screen.
    if (!room_is_active()) {
        return;
    }
    lvgl_port_lock(0);
    if (s_voice_active && !s_disconnecting && hint_label) {
        lv_label_set_text(hint_label, "Hold to disconnect...");
        lv_obj_set_style_text_color(hint_label, lv_color_hex(0xFFFFFF), 0); // white = "we see you"
        lv_obj_clear_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(hint_label);
    }
    lvgl_port_unlock();
}

void ui_knob_hold_end(void)
{
    // Same gate as ui_knob_hold_start — only revert text inside an active
    // session. Outside a session this would otherwise un-hide the hint
    // with stale text.
    if (!room_is_active()) {
        return;
    }
    lvgl_port_lock(0);
    if (s_voice_active && !s_disconnecting && hint_label) {
        lv_label_set_text(hint_label, "Hold knob to disconnect");
        lv_obj_set_style_text_color(hint_label, lv_color_hex(0xAAAAAA), 0); // grey
    }
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
    // Don't call update_hint_label() here — its s_voice_active gating would
    // re-hide the "Connecting..." hint between the CONNECTING and CONNECTED
    // state changes. ui_wifi_connecting() / ui_set_voice_active() own the
    // hint visibility directly.
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

    // We're now back on the idle home screen. End any in-flight disconnect
    // animation and hide the hint slot.
    if (s_disconnecting) {
        stop_disconnecting_anim();
        s_disconnecting = false;
        if (hint_label) lv_obj_add_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
    }

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
