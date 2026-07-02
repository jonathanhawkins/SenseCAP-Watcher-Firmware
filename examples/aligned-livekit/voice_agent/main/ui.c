#include "ui.h"
#include "esp_lvgl_port.h"
#include "wifi_scan.h"
#include "wifi_setup.h"
#include "esp_log.h"
#include "example.h"
#include "board.h"  // board_get_battery_percent / board_is_charging / board_is_battery_present

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
static lv_obj_t *battery_status_label = NULL; // Battery "NN%" (⚡ + green when charging, red when low)
static lv_obj_t *hint_label = NULL;        // "Hold knob to disconnect" hint (under status bar)
static lv_obj_t *knob_progress_bar = NULL; // Fills 0→100% across BUTTON_LONG_PRESS_MS while held
static lv_obj_t *failure_label = NULL;     // "Auth failed — re-pair" (centered, red)
static lv_obj_t *failure_hint  = NULL;     // "Press knob to retry"  (under failure_label, grey)
static bool s_voice_active = false;        // Track voice chat state
static bool s_last_wifi_connected = false; // Track WiFi state for change detection
static bool s_disconnecting = false;       // Mid-disconnect — keep "Disconnecting..." hint up
static bool s_disconnecting_anim_active = false;
static lv_anim_t s_disconnecting_anim;

// Live-meeting UI widgets. These overlay the home orb (kept as wallpaper)
// during a silent transcription session. See ui_meeting_start/end.
static lv_obj_t *meeting_btn = NULL;               // Home-screen meeting button (mic icon)
static lv_obj_t *meeting_btn_icon = NULL;          // mic image child of meeting_btn
static lv_obj_t *meeting_rec_label = NULL;         // "● REC" indicator (opacity-pulsed)
static lv_obj_t *meeting_transcript_label = NULL;  // latest transcript line(s)
static lv_obj_t *meeting_coach_card = NULL;        // coach suggestion card
static lv_obj_t *meeting_coach_label = NULL;
static lv_obj_t *meeting_end_btn = NULL;           // "End" button (bottom)
static lv_obj_t *meeting_end_label = NULL;
static lv_timer_t *meeting_coach_timer = NULL;     // one-shot auto-hide for coach card
static lv_anim_t s_rec_anim;
static bool s_rec_anim_active = false;
static bool s_meeting_ui_active = false;           // gates wifi_check_timer fighting us
// Rolling last-two transcript lines (older, newer) for the on-screen display.
static char s_tx_line_old[160] = {0};
static char s_tx_line_new[160] = {0};

// Forward declarations
static void wifi_btn_event_cb(lv_event_t *e);
static void wifi_check_timer_cb(lv_timer_t *timer);
static void create_wifi_button(void);
static void update_wifi_button_visibility(void);
static void create_status_bar(void);
static void update_status_bar(void);
static void update_battery_status(void);
static void create_hint_label(void);
static void update_hint_label(void);
static void create_meeting_button(void);
static void meeting_button_set_enabled_locked(bool enabled);
static void meeting_btn_event_cb(lv_event_t *e);
static void meeting_end_btn_event_cb(lv_event_t *e);
static void show_home_chrome_locked(void);  // home buttons/status bar; assumes LVGL lock held

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

// White microphone glyph for the Live Meeting button (main/mic_icon.c).
extern const lv_img_dsc_t mic_icon;

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

// Watchdog timer — fires if the normal disconnect flow (handle_long_release →
// leave_room → vTaskDelay(500) → ui_show_wifi_button) doesn't clear the
// "Disconnecting..." hint within DISCONNECT_WATCHDOG_MS. Two cases this
// catches:
//   1. `livekit_room_destroy()` hangs inside leave_room → button_task is
//      blocked and ui_show_wifi_button is never reached. The UI alone
//      recovers via this timer (LVGL task runs independent of button_task).
//   2. Some intermediate path bypassed ui_show_wifi_button (a new code path
//      we haven't anticipated). Same recovery.
// Cancelled by ui_show_wifi_button and ui_set_voice_active(true) — both of
// which are the "we're past disconnecting" states.
#define DISCONNECT_WATCHDOG_MS 3000
static lv_timer_t *s_disconnect_watchdog = NULL;

static void cancel_disconnect_watchdog(void)
{
    if (s_disconnect_watchdog) {
        lv_timer_del(s_disconnect_watchdog);
        s_disconnect_watchdog = NULL;
    }
}

static void disconnect_watchdog_cb(lv_timer_t *timer)
{
    // Always one-shot — delete first so a re-entrant ui_disconnecting() can
    // re-arm cleanly. We never want this firing twice on the same stuck state.
    if (s_disconnect_watchdog == timer) {
        s_disconnect_watchdog = NULL;
    }
    lv_timer_del(timer);

    if (!s_disconnecting) {
        // Normal flow already cleared it — nothing to do.
        return;
    }

    ESP_LOGW(TAG, "[ui] Disconnect watchdog firing — force-clearing stuck "
                  "\"Disconnecting...\" UI state after %d ms", DISCONNECT_WATCHDOG_MS);
    stop_disconnecting_anim();
    s_disconnecting = false;
    s_voice_active = false;
    if (hint_label) lv_obj_add_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
    if (knob_progress_bar) lv_obj_add_flag(knob_progress_bar, LV_OBJ_FLAG_HIDDEN);
    update_status_bar();
}

static void arm_disconnect_watchdog(void)
{
    cancel_disconnect_watchdog();
    s_disconnect_watchdog = lv_timer_create(disconnect_watchdog_cb,
                                             DISCONNECT_WATCHDOG_MS, NULL);
    if (s_disconnect_watchdog) {
        lv_timer_set_repeat_count(s_disconnect_watchdog, 1);
    }
}

// Opacity pulse for the voice-call status icon while CONNECTING. Same
// pattern as the disconnecting hint above and the wifi-scan label — we
// can't shake the icon (any spatial translation tears under the 40-line
// strip renderer per watcher-ui.md) so we pulse its alpha instead.
static bool s_voice_icon_anim_active = false;
static lv_anim_t s_voice_icon_anim;

static void voice_icon_opa_cb(void *var, int32_t val)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)val, LV_PART_MAIN);
}

static void start_voice_icon_connecting_anim(void)
{
    if (voice_status_icon == NULL || s_voice_icon_anim_active) {
        return;
    }
    lv_anim_init(&s_voice_icon_anim);
    lv_anim_set_var(&s_voice_icon_anim, voice_status_icon);
    lv_anim_set_exec_cb(&s_voice_icon_anim, (lv_anim_exec_xcb_t)voice_icon_opa_cb);
    // 40% → 100% gives a clear "fade and breathe" without disappearing.
    // 600 ms each way ≈ 1.2 s full cycle — slower than the disconnect
    // pulse (800 ms) so it reads as patient "trying" rather than urgent.
    lv_anim_set_values(&s_voice_icon_anim, LV_OPA_40, LV_OPA_COVER);
    lv_anim_set_time(&s_voice_icon_anim, 600);
    lv_anim_set_playback_time(&s_voice_icon_anim, 600);
    lv_anim_set_repeat_count(&s_voice_icon_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&s_voice_icon_anim);
    s_voice_icon_anim_active = true;
}

static void stop_voice_icon_connecting_anim(void)
{
    if (s_voice_icon_anim_active && voice_status_icon) {
        lv_anim_del(voice_status_icon, NULL);
        lv_obj_set_style_opa(voice_status_icon, LV_OPA_COVER, LV_PART_MAIN);
    }
    s_voice_icon_anim_active = false;
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

    // Clear any stale "Disconnecting..." state from a prior session — the
    // opacity-pulse anim would otherwise keep flickering the new
    // "Connecting..." text. Also cancel the disconnect watchdog since the
    // user reached this path on their own.
    cancel_disconnect_watchdog();
    if (s_disconnecting) {
        stop_disconnecting_anim();
        s_disconnecting = false;
    }
    // Hide the boot scroll label, but KEEP the listening orb visible — it is
    // the Aligned-logo background of the home screen. The user wants only the
    // boot text gone, not the wallpaper.
    if (label) lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);

    // Grey OUT the on-screen meeting (mic) button the moment a session starts
    // connecting. While a LiveKit voice session is up, the user must not be
    // able to tap the mic button and start a meeting recording. We dim +
    // disable (not destroy) — the widget is reused; ui_set_voice_active(false)
    // re-enables it on disconnect/failure. NULL-safe inside the helper.
    meeting_button_set_enabled_locked(false);

    // Pulse the voice-call icon while we're connecting. Tear-safe
    // opacity animation only (no spatial motion — see watcher-ui.md).
    // Stopped by ui_set_voice_active(true) on CONNECTED, by
    // ui_connection_failed() on FAILED, and by ui_show_wifi_button() on
    // return to idle.
    start_voice_icon_connecting_anim();

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

    // Arm the recovery watchdog. If handle_long_release → leave_room hangs
    // or some path bypasses ui_show_wifi_button, this timer force-clears the
    // stuck UI state after DISCONNECT_WATCHDOG_MS (3 s) so the user isn't
    // staring at a frozen "Disconnecting..." screen forever.
    arm_disconnect_watchdog();

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
    // Cancel watchdog + transient animations before the 500 ms vTaskDelay
    // that callers do after this returns — otherwise a fire during the
    // delay could touch hint_label / voice icon mid-style-change. (The
    // older version of this function called lv_obj_clean and had to stop
    // these to avoid UAF on freed widgets; we no longer destroy widgets,
    // but stopping anims here still gives a clean visual freeze for
    // shutdown.)
    cancel_disconnect_watchdog();
    stop_disconnecting_anim();
    stop_voice_icon_connecting_anim();
    s_disconnecting = false;

    // Stop the speaking→listening timer (one-shot, tied to voice activity)
    // and force the orb back to listening sprites. Without resetting
    // is_speaking, timer2_callback keeps cycling the SPEAKING images if
    // shutdown was triggered mid-utterance — the orb would visually argue
    // with the "Goodbye" hint. (ui_disconnecting does the same reset.)
    // timer2 itself is left running so the orb wallpaper stays gently
    // animated while the caller does its post-return shutdown delay —
    // matches the idle-screen behaviour the user had moments before.
    if (timer1) {
        lv_timer_del(timer1);
        timer1 = NULL;
    }
    is_speaking = false;
    s_voice_active = false;

    // DON'T lv_obj_clean(lv_scr_act()) — per .claude/rules/watcher-ui.md
    // the orb `img` IS the home wallpaper; nuking it exposes LVGL's
    // default white background. Instead hide the widgets that don't
    // belong on a goodbye screen and reuse the existing chrome (orb,
    // status bar, WiFi button) so the transition reads as a continuation
    // rather than a slam-cut to a blank screen.
    if (label)             lv_obj_add_flag(label,             LV_OBJ_FLAG_HIDDEN);
    if (failure_label)     lv_obj_add_flag(failure_label,     LV_OBJ_FLAG_HIDDEN);
    if (failure_hint)      lv_obj_add_flag(failure_hint,      LV_OBJ_FLAG_HIDDEN);
    if (knob_progress_bar) lv_obj_add_flag(knob_progress_bar, LV_OBJ_FLAG_HIDDEN);

    // s_voice_active=false + room closed → voice icon goes grey.
    update_status_bar();

    // Reuse the hint label slot under the status bar — the same position
    // that just showed "Hold knob to disconnect" / "Disconnecting…" /
    // "Connecting…". Bright white so it reads against the orb.
    create_hint_label();
    if (hint_label) {
        lv_anim_del(hint_label, NULL); // belt-and-suspenders: kill any anim still bound
        lv_obj_set_style_opa(hint_label, LV_OPA_COVER, LV_PART_MAIN);
        lv_label_set_text(hint_label, "Goodbye");
        lv_obj_set_style_text_color(hint_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_clear_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(hint_label);
    }

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
    // For a 412x412 round display, the usable width at top is narrower.
    // 150px (was 120) fits WiFi + call glyphs plus the "⚡100%" battery label
    // under SPACE_EVENLY; centered it spans x≈131..281, inside the round bezel.
    lv_obj_set_size(status_bar, 150, 30);
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

    // Battery percentage label (rightmost). Text + color are filled in by
    // update_battery_status(); "--%" is the pre-first-read placeholder.
    battery_status_label = lv_label_create(status_bar);
    if (battery_status_label) {
        lv_label_set_text(battery_status_label, "--%");
        lv_obj_set_style_text_font(battery_status_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(battery_status_label, lv_color_hex(0xFFFFFF), 0);
    }

    ESP_LOGI(TAG, "Status bar created");

    // Populate the battery reading immediately so it shows on first paint
    // rather than waiting for the first ~30s timer tick. We're under the
    // caller's LVGL lock here (create_status_bar is only called from locked
    // contexts), so the lv_label update is safe.
    update_battery_status();
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

/**
 * @brief Refresh the battery percentage label in the status bar.
 *
 * Reads the BSP state-of-charge (ADC voltage → quadratic curve-fit) and charge
 * state. Caller must hold the LVGL lock (this only runs from create_status_bar
 * and the wifi_check_timer lv_timer callback, both already locked). The bsp_*
 * reads themselves need no LVGL lock.
 *
 * - No battery present (USB-only, no cell): hide the label.
 * - Charging: prefix ⚡ and color green.
 * - Low (≤15%): red. Otherwise white.
 */
static void update_battery_status(void)
{
    if (battery_status_label == NULL) {
        return;
    }

    // NOTE: do NOT gate on board_is_battery_present(). The Watcher has a
    // built-in cell, and on this hardware BAT_DET reads HIGH (expander 0xffd9),
    // which the active-low presence check mistook for "absent" and hid the
    // label entirely. Always show the reading.
    //
    // Charging/USB state is a cheap expander read → refresh EVERY call (~2s) so
    // plug/unplug reflects quickly. The percentage needs a 10x ADC sample and
    // changes slowly → re-read only every ~30s (15 ticks) and cache between.
    static int s_cached_pct = -1;
    static uint8_t pct_tick = 0;
    if (s_cached_pct < 0 || pct_tick == 0) {
        s_cached_pct = (int)board_get_battery_percent(); // 0..100 (samples ADC 10x)
        ESP_LOGI(TAG, "Battery: %d%%", s_cached_pct);
    }
    pct_tick = (uint8_t)((pct_tick + 1) % 15);

    int pct = s_cached_pct;
    bool charging = board_is_charging(); // USB present (see board.c)

    char buf[16];
    if (charging) {
        // ⚡ + percent. If LV_SYMBOL_CHARGE renders as a box on-device, the
        // green color still conveys "on power".
        snprintf(buf, sizeof(buf), LV_SYMBOL_CHARGE "%d%%", pct);
    } else {
        snprintf(buf, sizeof(buf), "%d%%", pct);
    }
    lv_label_set_text(battery_status_label, buf);

    uint32_t color = charging ? 0x4CAF50               // green on USB power
                              : (pct <= 15 ? 0xF44336  // red when low on battery
                                           : 0xFFFFFF); // white otherwise
    lv_obj_set_style_text_color(battery_status_label, lv_color_hex(color), 0);
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

    // The on-screen meeting (mic) button tracks the INVERSE of voice-active:
    // while a live voice agent session is up it's greyed out + disabled so the
    // user can't accidentally start a meeting recording from inside the
    // session; on disconnect / failure it's re-enabled. The widget is reused
    // (watcher-ui.md), and in meeting mode this path isn't the one that owns
    // the button (ui_meeting_start/end hide it), so the NULL-checked toggle is
    // safe either way.
    meeting_button_set_enabled_locked(!active);

    // Stop the connecting-pulse on EITHER transition: success (active=true)
    // moves us to the steady green state, failure/disconnect (active=false)
    // restores the steady grey state.
    stop_voice_icon_connecting_anim();
    update_status_bar();

    // Directly own the hint widget here rather than going through
    // update_hint_label(). Going through that helper means the 2-second
    // wifi_check_timer can re-hide the hint between transient states
    // (e.g. during CONNECTING when s_voice_active is still false).
    if (hint_label) {
        if (active) {
            // Connected — restore the "Hold knob to disconnect" hint and
            // cancel any mid-disconnect animation (re-connect supersedes).
            cancel_disconnect_watchdog();
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

    // Stop the connecting-pulse — we're no longer trying.
    stop_voice_icon_connecting_anim();

    // Cancel any in-flight disconnect watchdog + reset the disconnecting
    // state. Without this, if a disconnect was in progress when the FAILED
    // state arrived (e.g. user aborted a stalled CONNECTING via long press,
    // leave_room ran, then the server signalled FAILED before
    // ui_show_wifi_button could land), the 3 s watchdog would fire later
    // and log a misleading "force-clearing stuck Disconnecting... UI state"
    // even though the device is showing a clear failure overlay.
    cancel_disconnect_watchdog();
    if (s_disconnecting) {
        stop_disconnecting_anim();
        s_disconnecting = false;
    }

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

// Create the hold-progress bar on first use. Lazy so the idle screen
// never carries the widget; LVGL heap is 32 KB and we'd rather only
// pay for it during an active voice session.
//
// Position chosen to sit ~8 px below the hint_label (font_montserrat_14
// at y=60), 140 px wide, 5 px tall. The bar is horizontal so its growth
// edge advances in x — but values are mutated only under lvgl_port_lock,
// so a single frame's strips all read the same width. See
// .claude/rules/watcher-ui.md for the strip-render tearing model that
// rules this safe (and the spinner unsafe).
static void ensure_knob_progress_bar(void)
{
    if (knob_progress_bar != NULL) {
        return;
    }
    knob_progress_bar = lv_bar_create(lv_scr_act());
    if (knob_progress_bar == NULL) {
        ESP_LOGW(TAG, "Failed to create knob progress bar");
        return;
    }
    lv_obj_set_size(knob_progress_bar, 140, 5);
    lv_obj_align(knob_progress_bar, LV_ALIGN_TOP_MID, 0, 82);
    lv_bar_set_range(knob_progress_bar, 0, 100);
    lv_bar_set_value(knob_progress_bar, 0, LV_ANIM_OFF);

    // Background track — dark grey so the white fill reads cleanly.
    lv_obj_set_style_bg_color(knob_progress_bar, lv_color_hex(0x404040), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(knob_progress_bar,   LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_radius(knob_progress_bar,   3,                       LV_PART_MAIN);
    lv_obj_set_style_border_width(knob_progress_bar, 0,                  LV_PART_MAIN);

    // Indicator (fill) — starts white to match the "Hold to disconnect..."
    // hint colour. ui_knob_hold_ready_* swap this to green / amber as the
    // user crosses each threshold.
    lv_obj_set_style_bg_color(knob_progress_bar, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(knob_progress_bar,   LV_OPA_COVER,           LV_PART_INDICATOR);
    lv_obj_set_style_radius(knob_progress_bar,   3,                       LV_PART_INDICATOR);

    lv_obj_add_flag(knob_progress_bar, LV_OBJ_FLAG_HIDDEN);
}

void ui_knob_hold_start(void)
{
    // Show the hold UX in two contexts:
    //   - Voice room active (CONNECTING / RECONNECTING / CONNECTED):
    //     hint reads "Hold to disconnect..." — release at 1.75 s drops
    //     the room, release at 5 s+ goes to deep sleep instead.
    //   - Idle home (no room): hint reads "Hold to connect..." — release
    //     under 1 s is a no-op (anti-accidental), release at 1 s+ joins the
    //     room, release at 5 s+ goes to deep sleep. button_task drives the
    //     connect -> sleep transition via ui_knob_hold_ready_connect() and
    //     ui_knob_hold_enter_sleep_phase().
    // Both contexts get a progress bar so the hold has visible feedback.
    // (Earlier this function early-returned when the room wasn't active,
    // leaving idle-screen holds with no UI feedback at all until "Goodbye"
    // appeared.)
    lvgl_port_lock(0);
    if (!s_disconnecting && hint_label) {
        const bool room_active = room_is_active();
        lv_label_set_text(hint_label, room_active ? "Hold to disconnect..." : "Hold to connect...");
        lv_obj_set_style_text_color(hint_label, lv_color_hex(0xFFFFFF), 0); // white = "we see you"
        lv_obj_clear_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(hint_label);

        // Reset progress bar to empty white + show it. Fill grows as the
        // button_task ticks call ui_knob_hold_progress() while the knob
        // is held.
        ensure_knob_progress_bar();
        if (knob_progress_bar) {
            lv_bar_set_value(knob_progress_bar, 0, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(knob_progress_bar, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
            lv_obj_clear_flag(knob_progress_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(knob_progress_bar);
        }
    }
    lvgl_port_unlock();
}

void ui_knob_hold_end(void)
{
    lvgl_port_lock(0);
    if (!s_disconnecting && hint_label) {
        if (room_is_active()) {
            // Voice room: revert to steady-state "Hold knob to disconnect"
            // grey hint. If the user actually crossed the disconnect
            // threshold, ui_disconnecting() will overwrite this almost
            // immediately, so the intermediate frame is imperceptible.
            lv_label_set_text(hint_label, "Hold knob to disconnect");
            lv_obj_set_style_text_color(hint_label, lv_color_hex(0xAAAAAA), 0); // grey
        } else {
            // Idle home: hide the hint entirely so the orb stays clean.
            // If the user crossed the sleep threshold, ui_powering_off()
            // will repaint this label with "Goodbye" in the next ~ms.
            // NOTE: this assumes the idle-home hint slot is owned solely
            // by the knob-hold UX. If a future feature shows hint_label
            // from another idle-context path (e.g. a transient toast), a
            // hold+release would erase it — gate this hide accordingly.
            lv_obj_add_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    // Always hide the progress bar on release — irrespective of session
    // state, we don't want a stale fill stuck on screen.
    if (knob_progress_bar) {
        lv_obj_add_flag(knob_progress_bar, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

void ui_knob_hold_progress(uint8_t pct)
{
    // Called by button_task every BUTTON_POLL_MS (25 ms) while the knob
    // is held. `pct` is clamped to 0–100; button_task picks the right
    // mapping based on room state:
    //   - Room active: held_ms / BUTTON_LONG_PRESS_MS so the fill reaches
    //     100% at the disconnect threshold (1.75 s).
    //   - Idle home: held_ms / BUTTON_SLEEP_MS so the fill reaches 100%
    //     at the sleep threshold (5 s).
    //
    // Tearing model: this runs under lvgl_port_lock, which the LVGL
    // refresh task also takes for the full multi-strip render cycle.
    // The bar's value therefore stays constant across all strips of
    // one frame — no inter-strip drift even though the indicator is
    // horizontal geometry. Per .claude/rules/watcher-ui.md the unsafe
    // case is animation-engine-driven motion (lv_spinner et al), not
    // app-driven mutations through the port lock.
    if (pct > 100) pct = 100;

    lvgl_port_lock(0);
    if (!s_disconnecting) {
        ensure_knob_progress_bar();
        if (knob_progress_bar) {
            lv_bar_set_value(knob_progress_bar, pct, LV_ANIM_OFF);
            lv_obj_clear_flag(knob_progress_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lvgl_port_unlock();
}

void ui_knob_hold_ready_disconnect(void)
{
    // Fired by button_task when held_ms crosses BUTTON_LONG_PRESS_MS (2 s).
    // The disconnect itself runs on release, so the user needs to know
    // they can let go now — otherwise they'll keep holding and risk
    // overshooting into the 5 s deep-sleep band.
    if (!room_is_active()) {
        return;
    }
    lvgl_port_lock(0);
    if (!s_disconnecting && hint_label) {
        lv_label_set_text(hint_label, "Release to disconnect");
        // Material green — same token used for the voice-active status icon,
        // so the colour reads as "good, go ahead" rather than alarm.
        lv_obj_set_style_text_color(hint_label, lv_color_hex(0x4CAF50), 0);
        lv_obj_clear_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(hint_label);

        // Bar reaches the threshold — pin to full + green to mirror text.
        if (knob_progress_bar) {
            lv_bar_set_value(knob_progress_bar, 100, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(knob_progress_bar, lv_color_hex(0x4CAF50), LV_PART_INDICATOR);
        }
    }
    lvgl_port_unlock();
}

void ui_knob_hold_ready_sleep(void)
{
    // Fired by button_task when held_ms crosses BUTTON_SLEEP_MS (5 s).
    // Past this point a release runs handle_deep_sleep(), not leave_room.
    // The amber colour distinguishes "you've gone past the disconnect
    // window and into the sleep window."
    //
    // Works for both contexts: room-active (just past the disconnect
    // threshold) and idle-home (this IS the only meaningful threshold).
    lvgl_port_lock(0);
    if (!s_disconnecting && hint_label) {
        lv_label_set_text(hint_label, "Release for sleep");
        lv_obj_set_style_text_color(hint_label, lv_color_hex(0xFFB300), 0); // amber
        lv_obj_clear_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(hint_label);

        // Past the disconnect threshold — bar stays full, swaps to amber.
        if (knob_progress_bar) {
            lv_bar_set_value(knob_progress_bar, 100, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(knob_progress_bar, lv_color_hex(0xFFB300), LV_PART_INDICATOR);
        }
    }
    lvgl_port_unlock();
}

void ui_knob_hold_ready_connect(void)
{
    // Idle home only. Fired by button_task when the hold crosses
    // BUTTON_CONNECT_MS (1 s). The join itself runs on release, so the user
    // needs to know they can let go NOW to connect — otherwise they keep
    // holding and overshoot toward sleep. Green = "good, go ahead", the same
    // token ui_knob_hold_ready_disconnect() uses for its release cue.
    if (room_is_active()) {
        return;  // disconnect/sleep flow owns the hint when a room is up
    }
    lvgl_port_lock(0);
    if (!s_disconnecting && hint_label) {
        lv_label_set_text(hint_label, "Release to connect");
        lv_obj_set_style_text_color(hint_label, lv_color_hex(0x4CAF50), 0); // green
        lv_obj_clear_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(hint_label);

        // Bar reaches the connect threshold — pin full + green to mirror text.
        if (knob_progress_bar) {
            lv_bar_set_value(knob_progress_bar, 100, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(knob_progress_bar, lv_color_hex(0x4CAF50), LV_PART_INDICATOR);
        }
    }
    lvgl_port_unlock();
}

void ui_knob_hold_enter_sleep_phase(void)
{
    // Idle home only. Fired once by button_task after the green
    // "Release to connect" dwell, when a hold continues past
    // BUTTON_CONNECT_MS + BUTTON_CONNECT_DWELL_MS on its way to the 5 s sleep
    // threshold. This is the visible hand-off from the connect gesture to the
    // sleep gesture: swap the hint to amber "Keep holding for sleep..." and
    // reset the bar to empty amber so ui_knob_hold_progress() can refill it
    // toward sleep. (A release before 5 s still connects — see button_task.)
    if (room_is_active()) {
        return;
    }
    lvgl_port_lock(0);
    if (!s_disconnecting && hint_label) {
        lv_label_set_text(hint_label, "Keep holding for sleep...");
        lv_obj_set_style_text_color(hint_label, lv_color_hex(0xFFB300), 0); // amber
        lv_obj_clear_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(hint_label);

        // Restart the fill from empty in amber — the refill is the countdown
        // toward sleep. ui_knob_hold_progress() keeps the colour as it advances.
        if (knob_progress_bar) {
            lv_obj_set_style_bg_color(knob_progress_bar, lv_color_hex(0xFFB300), LV_PART_INDICATOR);
            lv_bar_set_value(knob_progress_bar, 0, LV_ANIM_OFF);
            lv_obj_clear_flag(knob_progress_bar, LV_OBJ_FLAG_HIDDEN);
        }
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
    // During a live meeting the home buttons are intentionally hidden — don't
    // let the visibility refresh un-hide them.
    if (s_meeting_ui_active) {
        return;
    }
    update_wifi_button_visibility();
    update_status_bar();
    // Don't call update_hint_label() here — its s_voice_active gating would
    // re-hide the "Connecting..." hint between the CONNECTING and CONNECTED
    // state changes. ui_wifi_connecting() / ui_set_voice_active() own the
    // hint visibility directly.

    // Refresh the battery indicator every tick (~2s) so charging/USB state is
    // responsive on plug/unplug. update_battery_status() internally throttles
    // the costly ADC % sample to ~30s; the charge glyph/color updates each call.
    update_battery_status();
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

// Body of ui_show_wifi_button, assuming the LVGL lock is already held. Split
// out so ui_meeting_end() can restore the home chrome without re-locking the
// (non-recursive) port mutex.
static void show_home_chrome_locked(void)
{
    s_meeting_ui_active = false;

    // Restore the Aligned orb wallpaper (hidden during a meeting for readability).
    if (img) lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);

    create_wifi_button();
    update_wifi_button_visibility();
    create_meeting_button();
    if (meeting_btn) lv_obj_clear_flag(meeting_btn, LV_OBJ_FLAG_HIDDEN);

    // Create status bar (always visible when WiFi button system is active)
    create_status_bar();
    update_status_bar();

    // Back to idle home — make sure any leftover connecting-pulse is off.
    // Normal flow stops it via ui_set_voice_active or ui_connection_failed,
    // but this is a safety net if we route back to idle without those.
    stop_voice_icon_connecting_anim();

    // We're now back on the idle home screen. End any in-flight disconnect
    // animation and hide the hint slot. Cancel the disconnect watchdog if it
    // hasn't fired yet — normal flow reached us, no need for the safety net.
    cancel_disconnect_watchdog();
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
}

/**
 * @brief Initialize and show the WiFi button (call after ui_init or ui_listening)
 */
void ui_show_wifi_button(void)
{
    lvgl_port_lock(0);
    show_home_chrome_locked();
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

//=============================================================================
// Live Meeting (silent transcription) UI
//=============================================================================

static void meeting_btn_event_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Live Meeting button pressed");
    // start_meeting() does blocking HTTP + room setup — defer to button_task so
    // we don't stall the LVGL render task. See example.c::service_meeting_requests.
    request_start_meeting();
}

static void meeting_end_btn_event_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Meeting End button pressed");
    request_stop_meeting();
}

/**
 * @brief Create the "Live Meeting" button on the home screen (far left).
 *
 * Placed at LEFT_MID where the round screen is full-width (no edge clipping).
 * Tapping it starts a silent live-meeting transcription session.
 */
static void create_meeting_button(void)
{
    if (meeting_btn != NULL) {
        return;
    }
    meeting_btn = lv_btn_create(lv_scr_act());
    if (meeting_btn == NULL) {
        ESP_LOGE(TAG, "Failed to create Live Meeting button");
        return;
    }
    // Round, icon-only mic button at center-right, with the Aligned-logo
    // orange→cyan gradient (colors sampled from ALIGNED_LOGO_ONLY.webp).
    lv_obj_set_size(meeting_btn, 58, 58);
    lv_obj_align(meeting_btn, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_radius(meeting_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(meeting_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(meeting_btn, lv_color_hex(0xF0824D), 0);       // logo orange (start)
    lv_obj_set_style_bg_grad_color(meeting_btn, lv_color_hex(0x45C2DA), 0);  // logo cyan (end)
    lv_obj_set_style_bg_grad_dir(meeting_btn, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_border_width(meeting_btn, 0, 0);
    lv_obj_set_style_shadow_width(meeting_btn, 8, 0);
    lv_obj_set_style_shadow_color(meeting_btn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(meeting_btn, LV_OPA_30, 0);
    // Pressed: darken both gradient stops slightly for tactile feedback.
    lv_obj_set_style_bg_color(meeting_btn, lv_color_hex(0xD06A38), LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_color(meeting_btn, lv_color_hex(0x35A6BC), LV_STATE_PRESSED);

    // White microphone icon (custom image — LVGL 8.4 has no stock mic glyph).
    meeting_btn_icon = lv_img_create(meeting_btn);
    if (meeting_btn_icon) {
        lv_img_set_src(meeting_btn_icon, &mic_icon);
        lv_obj_center(meeting_btn_icon);
    }

    lv_obj_add_event_cb(meeting_btn, meeting_btn_event_cb, LV_EVENT_CLICKED, NULL);
    ESP_LOGI(TAG, "Live Meeting button created");
}

/**
 * @brief Enable / grey-out the home-screen meeting (mic) button.
 *
 * Disabled state: dim the whole button to ~40% opacity (the LV_PART_MAIN opa
 * cascades to the mic-icon child too) and clear LV_OBJ_FLAG_CLICKABLE so it
 * gives no press feedback and fires no CLICKED event. This is how the button
 * is turned off while a live voice-agent session is up, so a stray tap can't
 * start a meeting recording. The functional guard in
 * example.c::service_meeting_requests() (skips start_meeting() when
 * room_is_active()) remains as the backstop.
 *
 * Caller MUST already hold lvgl_port_lock — both call sites (ui_wifi_connecting,
 * ui_set_voice_active) are inside the lock. NULL-safe: meeting_btn isn't
 * created until the first idle-home render.
 */
static void meeting_button_set_enabled_locked(bool enabled)
{
    if (meeting_btn == NULL) {
        return;
    }
    if (enabled) {
        lv_obj_add_flag(meeting_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(meeting_btn, LV_OPA_COVER, LV_PART_MAIN);
    } else {
        lv_obj_clear_flag(meeting_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(meeting_btn, LV_OPA_40, LV_PART_MAIN); // ~40% = clearly dimmed
    }
}

static void stop_rec_anim(void)
{
    if (s_rec_anim_active && meeting_rec_label) {
        lv_anim_del(meeting_rec_label, NULL);
        lv_obj_set_style_opa(meeting_rec_label, LV_OPA_COVER, LV_PART_MAIN);
    }
    s_rec_anim_active = false;
}

static void coach_card_hide(void)
{
    if (meeting_coach_card) {
        lv_obj_add_flag(meeting_coach_card, LV_OBJ_FLAG_HIDDEN);
    }
}

static void meeting_coach_timer_cb(lv_timer_t *t)
{
    (void)t;
    coach_card_hide();
    // repeat_count was set to 1 → LVGL auto-deletes this timer after the cb.
    meeting_coach_timer = NULL;
}

static void coach_card_tap_cb(lv_event_t *e)
{
    (void)e;
    coach_card_hide();
    if (meeting_coach_timer) {
        lv_timer_del(meeting_coach_timer);
        meeting_coach_timer = NULL;
    }
}

void ui_meeting_start(void)
{
    lvgl_port_lock(0);
    s_meeting_ui_active = true;

    // Stop the WiFi-button poll timer so it doesn't un-hide the home buttons
    // (update_wifi_button_visibility clears the HIDDEN flag every 2 s).
    if (wifi_check_timer) {
        lv_timer_del(wifi_check_timer);
        wifi_check_timer = NULL;
    }

    // Readability: dark screen background + HIDE the Aligned orb so the white
    // transcript/coach text isn't competing with the logo. Restored on end.
    // (Hide, never lv_obj_clean — the orb widget is reused — per watcher-ui.md.)
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0A0A0B), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, LV_PART_MAIN);
    if (img) lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);

    // Hide idle home chrome.
    if (wifi_btn)      lv_obj_add_flag(wifi_btn, LV_OBJ_FLAG_HIDDEN);
    if (meeting_btn)   lv_obj_add_flag(meeting_btn, LV_OBJ_FLAG_HIDDEN);
    if (hint_label)    lv_obj_add_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
    if (label)         lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    if (failure_label) lv_obj_add_flag(failure_label, LV_OBJ_FLAG_HIDDEN);
    if (failure_hint)  lv_obj_add_flag(failure_hint,  LV_OBJ_FLAG_HIDDEN);

    // Reset rolling transcript lines.
    s_tx_line_old[0] = '\0';
    s_tx_line_new[0] = '\0';

    // "REC" indicator (static label + opacity pulse — tear-safe per watcher-ui.md).
    if (meeting_rec_label == NULL) {
        meeting_rec_label = lv_label_create(lv_scr_act());
    }
    if (meeting_rec_label) {
        lv_label_set_text(meeting_rec_label, LV_SYMBOL_AUDIO " REC");
        lv_obj_set_style_text_color(meeting_rec_label, lv_color_hex(0xE53935), 0);
        lv_obj_set_style_text_font(meeting_rec_label, &lv_font_montserrat_14, 0);
        lv_obj_align(meeting_rec_label, LV_ALIGN_TOP_MID, 0, 56);
        lv_obj_clear_flag(meeting_rec_label, LV_OBJ_FLAG_HIDDEN);
        // Clear any existing animation first so a repeated ui_meeting_start
        // (CONNECTED can fire more than once) doesn't stack infinite anims (M5).
        lv_anim_del(meeting_rec_label, NULL);
        lv_anim_init(&s_rec_anim);
        lv_anim_set_var(&s_rec_anim, meeting_rec_label);
        lv_anim_set_exec_cb(&s_rec_anim, disconnecting_label_opa_cb);
        lv_anim_set_values(&s_rec_anim, LV_OPA_COVER, LV_OPA_40);
        lv_anim_set_time(&s_rec_anim, 700);
        lv_anim_set_playback_time(&s_rec_anim, 700);
        lv_anim_set_repeat_count(&s_rec_anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&s_rec_anim);
        s_rec_anim_active = true;
    }

    // Transcript label (latest lines, centered, wrapped — no spatial motion).
    if (meeting_transcript_label == NULL) {
        meeting_transcript_label = lv_label_create(lv_scr_act());
    }
    if (meeting_transcript_label) {
        lv_label_set_long_mode(meeting_transcript_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(meeting_transcript_label, 300);
        lv_obj_set_style_text_align(meeting_transcript_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(meeting_transcript_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(meeting_transcript_label, &lv_font_montserrat_14, 0);
        lv_label_set_text(meeting_transcript_label, "Listening...");
        lv_obj_align(meeting_transcript_label, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(meeting_transcript_label, LV_OBJ_FLAG_HIDDEN);
    }

    // "End" button (bottom center).
    if (meeting_end_btn == NULL) {
        meeting_end_btn = lv_btn_create(lv_scr_act());
        if (meeting_end_btn) {
            lv_obj_set_size(meeting_end_btn, 120, 42);
            lv_obj_align(meeting_end_btn, LV_ALIGN_BOTTOM_MID, 0, -30);
            lv_obj_set_style_bg_color(meeting_end_btn, lv_color_hex(0x424242), 0);
            lv_obj_set_style_bg_opa(meeting_end_btn, LV_OPA_90, 0);
            lv_obj_set_style_radius(meeting_end_btn, 21, 0);
            lv_obj_set_style_bg_color(meeting_end_btn, lv_color_hex(0x616161), LV_STATE_PRESSED);
            meeting_end_label = lv_label_create(meeting_end_btn);
            lv_label_set_text(meeting_end_label, LV_SYMBOL_STOP " End");
            lv_obj_set_style_text_font(meeting_end_label, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(meeting_end_label, lv_color_hex(0xFFFFFF), 0);
            lv_obj_center(meeting_end_label);
            lv_obj_add_event_cb(meeting_end_btn, meeting_end_btn_event_cb, LV_EVENT_CLICKED, NULL);
        }
    } else {
        lv_obj_clear_flag(meeting_end_btn, LV_OBJ_FLAG_HIDDEN);
    }

    // Status bar stays; orb stays as background beneath everything.
    create_status_bar();
    update_status_bar();
    if (img)                       lv_obj_move_background(img);
    if (status_bar)                lv_obj_move_foreground(status_bar);
    if (meeting_rec_label)         lv_obj_move_foreground(meeting_rec_label);
    if (meeting_transcript_label)  lv_obj_move_foreground(meeting_transcript_label);
    if (meeting_end_btn)           lv_obj_move_foreground(meeting_end_btn);

    lvgl_port_unlock();
}

void ui_meeting_transcript_line(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    lvgl_port_lock(0);
    if (meeting_transcript_label) {
        // Roll: previous newer line becomes the older line, new text becomes newer.
        strncpy(s_tx_line_old, s_tx_line_new, sizeof(s_tx_line_old) - 1);
        s_tx_line_old[sizeof(s_tx_line_old) - 1] = '\0';
        strncpy(s_tx_line_new, text, sizeof(s_tx_line_new) - 1);
        s_tx_line_new[sizeof(s_tx_line_new) - 1] = '\0';

        char combined[332];
        if (s_tx_line_old[0] != '\0') {
            snprintf(combined, sizeof(combined), "%s\n%s", s_tx_line_old, s_tx_line_new);
        } else {
            snprintf(combined, sizeof(combined), "%s", s_tx_line_new);
        }
        lv_label_set_text(meeting_transcript_label, combined);

        // Diagnostic for "stops translating when full": watch the LVGL heap
        // (32 KB pool). If free trends toward 0, the freeze is heap exhaustion.
        static int s_tx_count = 0;
        if ((++s_tx_count % 5) == 0) {
            lv_mem_monitor_t mon;
            lv_mem_monitor(&mon);
            ESP_LOGI(TAG, "[meeting] tx#%d lvgl_free=%u used=%d%% frag=%d%%",
                     s_tx_count, (unsigned)mon.free_size, (int)mon.used_pct, (int)mon.frag_pct);
        }
    }
    lvgl_port_unlock();
}

void ui_meeting_coach_card(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    lvgl_port_lock(0);

    if (meeting_coach_card == NULL) {
        meeting_coach_card = lv_obj_create(lv_scr_act());
        if (meeting_coach_card) {
            // 412px round AMOLED: the usable chord narrows toward the bottom.
            // 300px keeps the card's lower corners inside the circle at this
            // y-offset (a 320px card sat right at the edge).
            lv_obj_set_size(meeting_coach_card, 300, LV_SIZE_CONTENT);
            lv_obj_align(meeting_coach_card, LV_ALIGN_BOTTOM_MID, 0, -82);
            lv_obj_set_style_bg_color(meeting_coach_card, lv_color_hex(0x1E88E5), 0);
            lv_obj_set_style_bg_opa(meeting_coach_card, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(meeting_coach_card, 14, 0);
            lv_obj_set_style_pad_all(meeting_coach_card, 10, 0);
            lv_obj_set_style_border_width(meeting_coach_card, 0, 0);
            lv_obj_clear_flag(meeting_coach_card, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_event_cb(meeting_coach_card, coach_card_tap_cb, LV_EVENT_CLICKED, NULL);

            meeting_coach_label = lv_label_create(meeting_coach_card);
            lv_label_set_long_mode(meeting_coach_label, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(meeting_coach_label, 276);  // 300 card − 2×10 pad − slack
            lv_obj_set_style_text_color(meeting_coach_label, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_font(meeting_coach_label, &lv_font_montserrat_14, 0);
        }
    }
    if (meeting_coach_card && meeting_coach_label) {
        lv_label_set_text(meeting_coach_label, text);
        lv_obj_clear_flag(meeting_coach_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(meeting_coach_card);

        // Auto-hide after 12 s. Replace any pending timer.
        if (meeting_coach_timer) {
            lv_timer_del(meeting_coach_timer);
            meeting_coach_timer = NULL;
        }
        meeting_coach_timer = lv_timer_create(meeting_coach_timer_cb, 12000, NULL);
        if (meeting_coach_timer) {
            lv_timer_set_repeat_count(meeting_coach_timer, 1);
        }
    }

    lvgl_port_unlock();
}

void ui_meeting_end(void)
{
    lvgl_port_lock(0);

    stop_rec_anim();
    if (meeting_coach_timer) {
        lv_timer_del(meeting_coach_timer);
        meeting_coach_timer = NULL;
    }
    if (meeting_rec_label)        lv_obj_add_flag(meeting_rec_label, LV_OBJ_FLAG_HIDDEN);
    if (meeting_transcript_label) lv_obj_add_flag(meeting_transcript_label, LV_OBJ_FLAG_HIDDEN);
    if (meeting_coach_card)       lv_obj_add_flag(meeting_coach_card, LV_OBJ_FLAG_HIDDEN);
    if (meeting_end_btn)          lv_obj_add_flag(meeting_end_btn, LV_OBJ_FLAG_HIDDEN);

    // Restore the idle home screen (re-shows WiFi + Live Meeting buttons,
    // restarts the WiFi poll timer). Reuses the lock-free helper.
    show_home_chrome_locked();

    lvgl_port_unlock();
}

//=============================================================================
// Plan-of-the-day picker
//
// Knob-selectable list of proposed time blocks, shown as a FULL-SCREEN modal.
// Driven by the agent's "voice-plan-day" artifact (example.c::on_data_received).
// Rotation moves the highlight (volume_control.c), a short knob press confirms
// (main.c button_task -> plan_confirm_selection() in example.c).
//
// The panel lives on lv_layer_top() — LVGL's system layer that always renders
// above every lv_scr_act() widget. This is deliberate: the hint label, status
// bar and wifi button are constantly re-raised via lv_obj_move_foreground on
// state changes, so a same-layer panel keeps losing the z-order fight and the
// underlying UI peeks out around it (real incident 2026-07-01: "Hold knob to
// disconnect" + mic + wifi visibly cut off behind the picker).
//
// watcher-ui.md compliance: overlays via LV_OBJ_FLAG_HIDDEN (never lv_obj_clean
// — the orb is wallpaper); rows are fixed geometry and selection scroll uses
// LV_ANIM_OFF (discrete jumps, no spatially-animated motion, so no tearing);
// capped rows + NULL-checked widgets (32 KB LVGL heap); selection is an
// in-place style change, not a moving cursor.
//
// Layout inside the 412x412 round panel (center 206, radius 206):
//   title  TOP_MID y=40                       (chord there fits ~244 px)
//   list   TOP_MID y=68, 288x280, scrollable  (band corners stay inside r=206)
//   "n/m"  BOTTOM_MID -44                     (position + "there's more" cue)
// Rows are 288x50 on a 56 px pitch: 5 full rows visible; the 6th row's top
// edge peeks in when count > 5, signalling scrollability.
//=============================================================================
#define PLAN_MAX_ROWS   UI_PLAN_MAX_BLOCKS
#define PLAN_ROW_W      288
#define PLAN_ROW_H      50
#define PLAN_ROW_PITCH  56
#define PLAN_LABEL_H    36   // 2 lines of montserrat_14 (16 px line height) + slack

static lv_obj_t *plan_panel = NULL;
static lv_obj_t *plan_list = NULL;   // scrollable viewport inside the panel
static lv_obj_t *plan_pos_label = NULL;  // "2 / 8" position indicator
static lv_obj_t *plan_rows[PLAN_MAX_ROWS] = {0};
static lv_obj_t *plan_row_labels[PLAN_MAX_ROWS] = {0};
static lv_timer_t *plan_autohide_timer = NULL;
static char plan_ids[PLAN_MAX_ROWS][64];
static int plan_count = 0;
static int plan_sel = 0;
static bool s_plan_active = false;

// Restyle rows so the selected one is highlighted, keep it scrolled into view,
// and refresh the "n / m" indicator. Caller holds the LVGL lock.
static void plan_restyle_rows_locked(void)
{
    for (int i = 0; i < plan_count; i++) {
        if (!plan_rows[i]) continue;
        bool sel = (i == plan_sel);
        lv_obj_set_style_bg_color(plan_rows[i], sel ? lv_color_hex(0x2E7D32) : lv_color_hex(0x222831), 0);
        lv_obj_set_style_border_width(plan_rows[i], sel ? 2 : 0, 0);
    }
    // Instant (non-animated) scroll — a discrete jump can't tear on the
    // 40-line partial-buffer pipeline, unlike a smooth scroll animation.
    if (plan_sel >= 0 && plan_sel < plan_count && plan_rows[plan_sel]) {
        lv_obj_scroll_to_view(plan_rows[plan_sel], LV_ANIM_OFF);
    }
    if (plan_pos_label) {
        if (plan_count > 1) {
            lv_label_set_text_fmt(plan_pos_label, "%d / %d", plan_sel + 1, plan_count);
            lv_obj_clear_flag(plan_pos_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(plan_pos_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Hide the picker. Caller holds the LVGL lock.
static void plan_hide_locked(void)
{
    s_plan_active = false;
    if (plan_autohide_timer) {
        lv_timer_del(plan_autohide_timer);
        plan_autohide_timer = NULL;
    }
    if (plan_panel) {
        lv_obj_add_flag(plan_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

// One-shot auto-dismiss. Runs on the LVGL task (timer handler holds the lock),
// so it hides directly rather than re-locking via ui_plan_hide().
static void plan_autohide_cb(lv_timer_t *t)
{
    (void)t;
    plan_autohide_timer = NULL;  // repeat_count==1 → LVGL auto-deletes after return
    plan_hide_locked();
}

void ui_plan_show(const ui_plan_block_t *blocks, int count)
{
    if (blocks == NULL || count <= 0) {
        return;
    }
    if (count > PLAN_MAX_ROWS) {
        count = PLAN_MAX_ROWS;
    }

    lvgl_port_lock(0);

    // Lazily build the panel once; reused across shows (orb stays as wallpaper).
    if (plan_panel == NULL) {
        // Full-screen opaque modal on the TOP layer — covers (and stays above)
        // the hint label, status bar, orb and wifi/mic widgets, which state
        // changes keep re-raising with lv_obj_move_foreground on lv_scr_act().
        plan_panel = lv_obj_create(lv_layer_top());
        if (plan_panel == NULL) {
            lvgl_port_unlock();
            return;
        }
        lv_obj_set_size(plan_panel, 412, 412);
        lv_obj_align(plan_panel, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(plan_panel, lv_color_hex(0x0E1116), 0);
        lv_obj_set_style_bg_opa(plan_panel, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(plan_panel, 0, 0);
        lv_obj_set_style_pad_all(plan_panel, 0, 0);
        lv_obj_set_style_border_width(plan_panel, 0, 0);
        lv_obj_clear_flag(plan_panel, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(plan_panel);
        if (title) {
            lv_label_set_text(title, "Pick a time (turn + press)");
            lv_obj_set_style_text_color(title, lv_color_hex(0xB9C2CC), 0);
            lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
            lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);
        }

        // Scrollable viewport. Sized/positioned so its corners stay inside the
        // round display (band y=68..348: corner distance <= 202 < r=206).
        plan_list = lv_obj_create(plan_panel);
        if (plan_list == NULL) {
            // Panel without a list is useless — hide it and bail.
            lv_obj_add_flag(plan_panel, LV_OBJ_FLAG_HIDDEN);
            lvgl_port_unlock();
            return;
        }
        lv_obj_set_size(plan_list, PLAN_ROW_W, 280);
        lv_obj_align(plan_list, LV_ALIGN_TOP_MID, 0, 68);
        lv_obj_set_style_bg_opa(plan_list, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(plan_list, 0, 0);
        lv_obj_set_style_pad_all(plan_list, 0, 0);
        lv_obj_set_style_radius(plan_list, 0, 0);
        lv_obj_set_scrollbar_mode(plan_list, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_scroll_dir(plan_list, LV_DIR_VER);

        // "n / m" position cue — doubles as the "there's more below" signal.
        plan_pos_label = lv_label_create(plan_panel);
        if (plan_pos_label) {
            lv_label_set_text(plan_pos_label, "");
            lv_obj_set_style_text_color(plan_pos_label, lv_color_hex(0x8A94A0), 0);
            lv_obj_set_style_text_font(plan_pos_label, &lv_font_montserrat_14, 0);
            lv_obj_align(plan_pos_label, LV_ALIGN_BOTTOM_MID, 0, -44);
        }

        // Fixed-pitch rows inside the viewport (no flex dependency, no spatial
        // motion). LVGL's scroll extent skips HIDDEN children, so spare rows
        // don't add empty scroll space (verified lv_obj_scroll.c, LVGL 8.4).
        for (int i = 0; i < PLAN_MAX_ROWS; i++) {
            plan_rows[i] = lv_obj_create(plan_list);
            if (plan_rows[i] == NULL) {
                break;  // heap exhausted — keep the rows we got, NULL-guarded below
            }
            lv_obj_set_size(plan_rows[i], PLAN_ROW_W, PLAN_ROW_H);
            lv_obj_set_pos(plan_rows[i], 0, i * PLAN_ROW_PITCH);
            lv_obj_set_style_radius(plan_rows[i], 10, 0);
            lv_obj_set_style_pad_left(plan_rows[i], 12, 0);
            lv_obj_set_style_pad_right(plan_rows[i], 12, 0);
            lv_obj_set_style_pad_top(plan_rows[i], 0, 0);
            lv_obj_set_style_pad_bottom(plan_rows[i], 0, 0);
            lv_obj_set_style_border_color(plan_rows[i], lv_color_hex(0x66FF99), 0);
            lv_obj_set_style_border_width(plan_rows[i], 0, 0);
            lv_obj_clear_flag(plan_rows[i], LV_OBJ_FLAG_SCROLLABLE);

            plan_row_labels[i] = lv_label_create(plan_rows[i]);
            if (plan_row_labels[i]) {
                // Fixed W+H + LONG_DOT = hard 2-line clamp with a real "..."
                // (auto-height labels never dot-truncate; they just keep
                // wrapping and bleed over the next row — the 2026-07-01 bug).
                lv_label_set_long_mode(plan_row_labels[i], LV_LABEL_LONG_DOT);
                lv_label_set_recolor(plan_row_labels[i], true);
                lv_obj_set_size(plan_row_labels[i], PLAN_ROW_W - 24, PLAN_LABEL_H);
                lv_obj_align(plan_row_labels[i], LV_ALIGN_LEFT_MID, 0, 0);
                lv_obj_set_style_text_color(plan_row_labels[i], lv_color_hex(0xFFFFFF), 0);
                lv_obj_set_style_text_font(plan_row_labels[i], &lv_font_montserrat_14, 0);
            }
        }
    }

    // Populate rows; hide unused. Cap at however many rows actually allocated.
    plan_count = count;
    plan_sel = 0;
    for (int i = 0; i < PLAN_MAX_ROWS; i++) {
        if (plan_rows[i] == NULL) {
            if (i < plan_count) plan_count = i;  // ran out of widgets — clamp
            break;
        }
        if (i < count) {
            const char *id = blocks[i].id ? blocks[i].id : "";
            strncpy(plan_ids[i], id, sizeof(plan_ids[i]) - 1);
            plan_ids[i][sizeof(plan_ids[i]) - 1] = '\0';

            const char *lbl = blocks[i].label ? blocks[i].label : "";
            const char *ttl = blocks[i].title ? blocks[i].title : "";
            // Titles are user text: escape '#' (LVGL recolor command char) by
            // doubling it, or a task like "fix #123" would eat the following
            // word as a color spec. 120 escaped chars > the ~70 that fit in
            // two lines, so the clamp below still owns truncation.
            char safe_ttl[120];
            size_t o = 0;
            for (const char *p = ttl; *p != '\0' && o < sizeof(safe_ttl) - 2; p++) {
                if (*p == '#') safe_ttl[o++] = '#';
                safe_ttl[o++] = *p;
            }
            safe_ttl[o] = '\0';
            // Mint-tinted time prefix via LVGL recolor. The markup sits at the
            // start of line 1, so LONG_DOT truncation (end of line 2) can never
            // split the #...# span. (Time labels are server-formatted, never '#'.)
            char row[176];
            snprintf(row, sizeof(row), "#A5E8B8 %s#  %s", lbl, safe_ttl);
            if (plan_row_labels[i]) {
                lv_label_set_text(plan_row_labels[i], row);
            }
            lv_obj_clear_flag(plan_rows[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(plan_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (plan_count <= 0) {
        // No rows could be created — bail without showing an empty panel.
        lv_obj_add_flag(plan_panel, LV_OBJ_FLAG_HIDDEN);
        s_plan_active = false;
        lvgl_port_unlock();
        return;
    }

    if (plan_list) {
        lv_obj_scroll_to_y(plan_list, 0, LV_ANIM_OFF);  // fresh show starts at the top
    }
    plan_restyle_rows_locked();
    lv_obj_clear_flag(plan_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(plan_panel);  // within lv_layer_top(), in case anything else lands there
    s_plan_active = true;

    // Auto-dismiss after 45 s of no interaction.
    if (plan_autohide_timer) {
        lv_timer_del(plan_autohide_timer);
        plan_autohide_timer = NULL;
    }
    plan_autohide_timer = lv_timer_create(plan_autohide_cb, 45000, NULL);
    if (plan_autohide_timer) {
        lv_timer_set_repeat_count(plan_autohide_timer, 1);
    }

    lvgl_port_unlock();
}

void ui_plan_hide(void)
{
    lvgl_port_lock(0);
    plan_hide_locked();
    lvgl_port_unlock();
}

bool ui_plan_is_active(void)
{
    // Plain read, no lock: called from the volume/button poll paths every few
    // ms. A benign race (one extra/missed routed rotation across the show/hide
    // edge) is harmless and not worth contending the LVGL lock for.
    return s_plan_active;
}

void ui_plan_move_selection(int delta)
{
    lvgl_port_lock(0);
    if (s_plan_active && plan_count > 0) {
        plan_sel += (delta > 0) ? 1 : -1;
        if (plan_sel < 0) plan_sel = plan_count - 1;       // wrap
        if (plan_sel >= plan_count) plan_sel = 0;
        plan_restyle_rows_locked();
        if (plan_autohide_timer) {
            lv_timer_reset(plan_autohide_timer);  // keep alive while interacting
        }
    }
    lvgl_port_unlock();
}

const char *ui_plan_get_selected_id(void)
{
    static char id[64];
    id[0] = '\0';
    lvgl_port_lock(0);
    if (s_plan_active && plan_sel >= 0 && plan_sel < plan_count) {
        strncpy(id, plan_ids[plan_sel], sizeof(id) - 1);
        id[sizeof(id) - 1] = '\0';
    }
    lvgl_port_unlock();
    return id[0] ? id : NULL;
}
