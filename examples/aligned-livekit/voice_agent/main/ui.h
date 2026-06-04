#ifndef UI_H
#define UI_H

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

void ui_init(void);
void ui_switch_speaking(void);
void ui_listening(void);
void ui_wifi_connecting(void);
void ui_disconnecting(void);
void ui_powering_off(void);

/**
 * @brief Enter the live-meeting (silent transcription) screen.
 *
 * Keeps the home orb as the wallpaper but overlays a recording indicator,
 * a latest-transcript line, and an "End" button. Hides the idle home buttons.
 * Called on CONNECTED when the session is a meeting (see example.c).
 */
void ui_meeting_start(void);

/**
 * @brief Update the on-screen transcript with the latest finalized line.
 *
 * Shows the most recent line(s) as a static label (no spatial motion — see
 * .claude/rules/watcher-ui.md). The full transcript lives in the web app.
 *
 * @param text Latest finalized transcript text (UTF-8, may be truncated).
 */
void ui_meeting_transcript_line(const char *text);

/**
 * @brief Show a coach suggestion card during a meeting.
 *
 * Static card near the bottom; auto-dismisses after a few seconds or on tap.
 *
 * @param text Coach suggestion display text.
 */
void ui_meeting_coach_card(const char *text);

/**
 * @brief Tear down the meeting screen and restore the idle home wallpaper.
 *
 * Safe to call multiple times. Hides meeting widgets and re-shows the home
 * buttons. Never calls lv_obj_clean (the orb is the wallpaper).
 */
void ui_meeting_end(void);

/**
 * @brief Show WiFi setup button at bottom of screen
 *
 * Button appears when WiFi is disconnected, hidden when connected.
 * Automatically checks WiFi status every 2 seconds.
 * Also creates the status bar with WiFi/voice indicators.
 */
void ui_show_wifi_button(void);

/**
 * @brief Hide and cleanup WiFi button
 */
void ui_hide_wifi_button(void);

/**
 * @brief Set voice chat active state and update UI indicators
 *
 * When active, shows green voice icon in status bar and
 * "Hold knob to disconnect" hint just below the status bar.
 *
 * @param active true when connected to voice agent, false when disconnected
 */
void ui_set_voice_active(bool active);

/**
 * @brief Show a connection-failure overlay with a user-readable reason.
 *
 * Displays @p reason_text centered in red with a "Press knob to retry"
 * hint below. The status bar and WiFi button stay visible. The overlay
 * auto-clears the next time ui_wifi_connecting() or ui_listening() runs.
 *
 * @param reason_text Short user-facing string (≤24 chars recommended).
 */
void ui_connection_failed(const char *reason_text);

/**
 * @brief Clear any connection-failure overlay if one is showing.
 *
 * Safe to call regardless of state. Called automatically when the user
 * retries a connection.
 */
void ui_clear_connection_failure(void);

/**
 * @brief Indicate that the user is currently pressing/holding the knob.
 *
 * Swaps the hint text to white so the user knows the press is registered:
 * "Hold to disconnect..." during an active voice session, "Hold to
 * connect..." on the idle home screen. Shows the hold-progress bar in both
 * contexts. Pair with ui_knob_hold_end() on the release edge.
 */
void ui_knob_hold_start(void);

/**
 * @brief Indicate the user released the knob.
 *
 * Reverts the hint to "Hold knob to disconnect" if the voice session
 * is still active and we're not already mid-disconnect. Hides the
 * hold-progress bar.
 */
void ui_knob_hold_end(void);

/**
 * @brief Update the hold-progress bar fill while the knob is held.
 *
 * @param pct 0–100, clamped. Mapped from held_ms / BUTTON_LONG_PRESS_MS
 *            in button_task so the bar reaches 100% exactly when the
 *            disconnect threshold trips.
 *
 * No-op outside an active voice session. Lazily creates the bar widget
 * on first call.
 */
void ui_knob_hold_progress(uint8_t pct);

/**
 * @brief Tell the user they've held long enough to trigger a disconnect.
 *
 * Fired once during a press, the first time the hold duration crosses
 * BUTTON_LONG_PRESS_MS (2 s). Swaps the hint to "Release to disconnect"
 * in green so the user knows releasing NOW will tear down the session
 * (no need to keep holding). No-op outside an active voice session.
 */
void ui_knob_hold_ready_disconnect(void);

/**
 * @brief Tell the user they've held long enough to trigger deep sleep.
 *
 * Fired once during a press, the first time the hold duration crosses
 * BUTTON_SLEEP_MS (5 s). Swaps the hint to "Release for sleep" in amber.
 * Past this point a release runs handle_deep_sleep() instead of leaving
 * the room — the colour change disambiguates the two outcomes.
 */
void ui_knob_hold_ready_sleep(void);

/**
 * @brief Idle home: tell the user they've held long enough to connect.
 *
 * Fired once when the hold crosses BUTTON_CONNECT_MS (1 s) on the idle home
 * screen. Swaps the hint to "Release to connect" in green and pins the bar
 * full — releasing NOW joins the voice room. A tap shorter than this is
 * ignored so a stray knob bump never starts a session. No-op in a voice
 * session (the disconnect flow owns the hint there).
 */
void ui_knob_hold_ready_connect(void);

/**
 * @brief Idle home: hand the hold UX off from "connect" to "sleep".
 *
 * Fired once when an idle-home hold continues past the green
 * "Release to connect" dwell (BUTTON_CONNECT_MS + BUTTON_CONNECT_DWELL_MS)
 * toward the 5 s sleep threshold. Swaps the hint to amber "Keep holding for
 * sleep..." and resets the bar to empty amber so ui_knob_hold_progress() can
 * refill it as the sleep countdown. No-op in a voice session.
 */
void ui_knob_hold_enter_sleep_phase(void);

//=============================================================================
// Plan-of-the-day picker (voice "plan my day")
//
// The agent publishes a "voice-plan-day" artifact; the firmware renders the
// proposed time blocks as a knob-selectable list overlaid on the home orb.
// Rotation moves the highlight (volume_control.c), a short knob press confirms
// (main.c button_task -> plan_confirm_selection() in example.c), and the
// selected block id is published back on the "plan_select" data topic so the
// agent books it. See .claude/rules/watcher-voice-flow.md + watcher-ui.md.
//=============================================================================

/** One selectable proposed time block. Strings are borrowed for the duration
 *  of the ui_plan_show() call only (copied internally as needed). */
typedef struct {
    const char *id;     ///< stable block id, round-tripped to the agent
    const char *label;  ///< pre-formatted local time, e.g. "9:00 AM"
    const char *title;  ///< e.g. "Focus: Write investor email"
} ui_plan_block_t;

/** Show/replace the plan picker with @p count blocks (capped internally).
 *  Safe to call from any task (self-locks LVGL). Overlays the orb wallpaper. */
void ui_plan_show(const ui_plan_block_t *blocks, int count);

/** Hide the plan picker if showing. Safe from any task; never lv_obj_clean. */
void ui_plan_hide(void);

/** True while the picker is visible (rotation/press are routed to it then). */
bool ui_plan_is_active(void);

/** Move the highlight by @p delta (>0 next, <0 previous; wraps). */
void ui_plan_move_selection(int delta);

/** Currently-highlighted block id, or NULL if none. Points at a static buffer
 *  valid until the next call. */
const char *ui_plan_get_selected_id(void);

#endif // UI_H
