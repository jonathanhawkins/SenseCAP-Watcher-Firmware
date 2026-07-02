#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/task.h"
#include "livekit.h"
#include "livekit_sandbox.h"
#include "media.h"
#include "board.h"
#include "example.h"
#include "aligned_client.h"
#include "ui.h"

static const char *TAG = "livekit_example";

static livekit_room_handle_t room_handle;
static bool agent_joined = false;
static bool s_leaving_room = false;  /* Re-entry guard for leave_room() */
// Guards the s_leaving_room check-and-set. leave_room() runs from BOTH
// button_task (main.c) and the console task (cmd.c) — separate tasks that can
// race. A plain check-then-set let both callers pass the guard and
// double-close/destroy the same handle (use-after-free). Same fix pattern as
// s_watchdog_mux below.
static portMUX_TYPE s_leave_mux = portMUX_INITIALIZER_UNLOCKED;
// Live-meeting mode: silent transcription session (vs. conversational voice).
// Set at connect time (after stale-handle cleanup) and read by the state/data
// callbacks to drive the meeting UI instead of the voice listening orb.
static bool s_meeting_mode = false;

// Touch-button → worker-task handoff. The "Live Meeting" / "End" buttons run
// their click callbacks in the LVGL render task, but start_meeting()/stop_meeting()
// do blocking HTTP and a room teardown — running those in the LVGL task would
// freeze rendering and trip the watchdog. So the callbacks just set a flag and
// button_task (a normal FreeRTOS task) services it via service_meeting_requests().
static volatile bool s_req_start_meeting = false;
static volatile bool s_req_stop_meeting = false;

// Agent-join watchdog. After CONNECTED state fires we start a timer; if no
// agent has joined the room by the time it elapses, surface "Agent
// unavailable — retry" instead of leaving the user staring at a silent
// listening orb. 8 s tolerates LiveKit Cloud cold starts (~5 s typical)
// while still catching the "no agent deployed" failure mode.
#define AGENT_JOIN_TIMEOUT_MS 8000
static TimerHandle_t s_agent_join_timer = NULL;
static bool s_agent_join_failed = false;
// Guards s_agent_join_timer. start/cancel run from multiple tasks (button_task
// via leave_room AND the LiveKit on_state_changed callback). Without this,
// two concurrent cancels both pass the NULL-check and call xTimerDelete on the
// same handle → double-free corrupts the FreeRTOS timer list (uxListRemove
// StoreProhibited crash, seen on meeting end 2026-05-22).
static portMUX_TYPE s_watchdog_mux = portMUX_INITIALIZER_UNLOCKED;

// Reconnect-stuck watchdog. The LiveKit SDK auto-reconnects on a dropped peer
// (RECONNECTING state). If that loop never completes — the cloud agent left and
// isn't coming back, or the media path is wedged — the device used to sit on
// "Connecting…" FOREVER with the knob unresponsive (observed: a ~19-hour lock,
// 2026-06-03). Bound it: if we stay in RECONNECTING past this, surface a retry
// prompt. Recovery is the SAME crash-safe path as the agent-join watchdog —
// flag the session dead + show retry; the actual leave_room() teardown is
// deferred to the next knob hold (connect_room_internal's had_agent_failure
// path), never run in the timer-service task. 25 s is generous vs LiveKit's own
// reconnect budget so we don't cut a legitimately-recovering session.
#define RECONNECT_TIMEOUT_MS 25000
static TimerHandle_t s_reconnect_timer = NULL;   // also guarded by s_watchdog_mux
static volatile bool s_reconnecting = false;     // true while SDK auto-reconnect is in flight

static void agent_join_timer_cb(TimerHandle_t t)
{
    (void)t;
    // Race: agent may have joined in the gap between timer fire and this
    // callback running. on_participant_info would have set agent_joined.
    if (agent_joined) {
        ESP_LOGI(TAG, "Agent join watchdog fired but agent already joined; ignoring");
        return;
    }
    ESP_LOGW(TAG, "Agent didn't join within %d ms — surfacing 'Agent unavailable'",
             AGENT_JOIN_TIMEOUT_MS);
    s_agent_join_failed = true;
    // ui_connection_failed takes the LVGL lock internally. ui_set_voice_active
    // (false) hides the "Hold knob to disconnect" hint and greys the audio icon.
    ui_set_voice_active(false);
    ui_connection_failed("Agent unavailable — retry");
}

static void start_agent_join_timer(void)
{
    // Atomically take ownership of any existing timer, then delete it OUTSIDE
    // the critical section (xTimer* calls queue commands and must not run in a
    // critical section). See s_watchdog_mux.
    TimerHandle_t old;
    taskENTER_CRITICAL(&s_watchdog_mux);
    old = s_agent_join_timer;
    s_agent_join_timer = NULL;
    taskEXIT_CRITICAL(&s_watchdog_mux);
    if (old != NULL) {
        xTimerStop(old, 0);
        xTimerDelete(old, 0);
    }

    TimerHandle_t t = xTimerCreate(
        "agent_join",
        pdMS_TO_TICKS(AGENT_JOIN_TIMEOUT_MS),
        pdFALSE,  // one-shot
        NULL,
        agent_join_timer_cb
    );
    if (t && xTimerStart(t, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start agent-join watchdog");
        xTimerDelete(t, 0);
        t = NULL;
    }
    taskENTER_CRITICAL(&s_watchdog_mux);
    s_agent_join_timer = t;
    taskEXIT_CRITICAL(&s_watchdog_mux);
}

static void cancel_agent_join_timer(void)
{
    // Atomically claim the handle so only ONE concurrent caller deletes it.
    TimerHandle_t t;
    taskENTER_CRITICAL(&s_watchdog_mux);
    t = s_agent_join_timer;
    s_agent_join_timer = NULL;
    taskEXIT_CRITICAL(&s_watchdog_mux);
    if (t != NULL) {
        xTimerStop(t, 0);
        xTimerDelete(t, 0);
    }
}

static void reconnect_timer_cb(TimerHandle_t t)
{
    (void)t;
    // Race: CONNECTED may have fired in the gap between the timer firing and
    // this callback running (it clears s_reconnecting). If so, do nothing.
    // We check a BOOL, not livekit_room_get_state(room_handle) — dereferencing
    // the handle here could race a concurrent leave_room() destroying it
    // (use-after-free). This mirrors agent_join_timer_cb's bool-only check.
    if (!s_reconnecting) {
        ESP_LOGI(TAG, "Reconnect watchdog fired but already recovered; ignoring");
        return;
    }
    ESP_LOGW(TAG, "Stuck reconnecting >%d ms — surfacing retry instead of a frozen screen",
             RECONNECT_TIMEOUT_MS);
    // Same crash-safe recovery as agent_join_timer_cb: do NOT call leave_room()
    // here (it blocks 500 ms and races peer_task — must run on a normal task,
    // not the timer-service task). Flag the session dead so room_is_active()
    // returns false; the next knob hold runs connect_room_internal, whose
    // had_agent_failure path tears the wedged handle down cleanly and reconnects.
    s_agent_join_failed = true;
    ui_set_voice_active(false);
    ui_connection_failed("Connection lost — retry");
}

static void start_reconnect_timer(void)
{
    // Mirror start_agent_join_timer's mux discipline: claim+delete any old timer
    // OUTSIDE the critical section, then create+start the new one.
    TimerHandle_t old;
    taskENTER_CRITICAL(&s_watchdog_mux);
    old = s_reconnect_timer;
    s_reconnect_timer = NULL;
    taskEXIT_CRITICAL(&s_watchdog_mux);
    if (old != NULL) {
        xTimerStop(old, 0);
        xTimerDelete(old, 0);
    }

    TimerHandle_t t = xTimerCreate(
        "reconnect_wd",
        pdMS_TO_TICKS(RECONNECT_TIMEOUT_MS),
        pdFALSE,  // one-shot
        NULL,
        reconnect_timer_cb
    );
    if (t && xTimerStart(t, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start reconnect watchdog");
        xTimerDelete(t, 0);
        t = NULL;
    }
    taskENTER_CRITICAL(&s_watchdog_mux);
    s_reconnect_timer = t;
    taskEXIT_CRITICAL(&s_watchdog_mux);
}

static void cancel_reconnect_timer(void)
{
    TimerHandle_t t;
    taskENTER_CRITICAL(&s_watchdog_mux);
    t = s_reconnect_timer;
    s_reconnect_timer = NULL;
    taskEXIT_CRITICAL(&s_watchdog_mux);
    if (t != NULL) {
        xTimerStop(t, 0);
        xTimerDelete(t, 0);
    }
}

/// Map a LiveKit failure-reason enum to a brief user-facing string.
/// Keep messages ≤24 chars so they wrap cleanly on the 412 px round screen.
static const char *describe_livekit_failure(livekit_failure_reason_t r)
{
    switch (r) {
        case LIVEKIT_FAILURE_REASON_UNREACHABLE:         return "LiveKit unreachable";
        case LIVEKIT_FAILURE_REASON_BAD_TOKEN:
        case LIVEKIT_FAILURE_REASON_UNAUTHORIZED:        return "Auth failed — re-pair";
        case LIVEKIT_FAILURE_REASON_RTC:                 return "Audio setup failed";
        case LIVEKIT_FAILURE_REASON_MAX_RETRIES:         return "Couldn't connect";
        case LIVEKIT_FAILURE_REASON_PING_TIMEOUT:
        case LIVEKIT_FAILURE_REASON_CONNECTION_TIMEOUT:  return "Connection timeout";
        case LIVEKIT_FAILURE_REASON_MEDIA_FAILURE:       return "Audio device error";
        case LIVEKIT_FAILURE_REASON_SERVER_SHUTDOWN:     return "Server restarted";
        case LIVEKIT_FAILURE_REASON_ROOM_DELETED:        return "Session ended";
        case LIVEKIT_FAILURE_REASON_NONE:                return "Connection failed";
        default:                                         return "Connection failed";
    }
}

/// Invoked when the room's connection state changes.
static void on_state_changed(livekit_connection_state_t state, void *ctx)
{
    ESP_LOGI(TAG, "Room state changed: %s", livekit_connection_state_str(state));

    // Update UI based on connection state
    switch (state)
    {
        case LIVEKIT_CONNECTION_STATE_CONNECTED:
            ESP_LOGI(TAG, "✅ Connected to LiveKit room!");
            s_reconnecting = false;
            cancel_reconnect_timer();  // reconnect completed (or never was reconnecting)
            if (s_meeting_mode) {
                // Silent meeting: show the recording/transcript screen instead
                // of the conversational listening orb + "hold to disconnect".
                ui_meeting_start();
            } else {
                ui_listening();  // Show listening animation - ready for voice
                ui_set_voice_active(true);  // Update status bar: voice active
            }
            // Start the agent-join watchdog. The meeting transcriber also joins
            // as an agent participant, so the watchdog applies to both modes.
            start_agent_join_timer();
            break;
        case LIVEKIT_CONNECTION_STATE_CONNECTING:
            ui_wifi_connecting();  // Show connecting state (initial connect)
            break;
        case LIVEKIT_CONNECTION_STATE_RECONNECTING:
            // Auto-reconnect in flight. Bound it — without this the device used
            // to wedge here forever (frozen "Connecting…", knob dead → 19h lock).
            ESP_LOGI(TAG, "Auto-reconnecting; arming %d ms recovery watchdog", RECONNECT_TIMEOUT_MS);
            s_reconnecting = true;
            ui_wifi_connecting();
            start_reconnect_timer();
            break;
        case LIVEKIT_CONNECTION_STATE_DISCONNECTED:
            cancel_agent_join_timer();
            cancel_reconnect_timer();
            s_reconnecting = false;
            ui_plan_hide();              // dismiss the plan picker if it was up
            ui_set_voice_active(false);  // Update status bar: voice inactive
            if (s_meeting_mode) {
                // End the meeting screen and return to the home wallpaper.
                ui_meeting_end();
            } else {
                ui_disconnecting();  // Show disconnected state
            }
            break;
        case LIVEKIT_CONNECTION_STATE_FAILED:
            ESP_LOGE(TAG, "❌ Connection failed!");
            cancel_agent_join_timer();
            cancel_reconnect_timer();
            s_reconnecting = false;
            ui_set_voice_active(false);  // Update status bar: voice inactive
            // Show a user-readable failure reason. The room handle is left in
            // place so livekit_room_get_failure_reason() works; join_room()
            // will tear it down on the next retry attempt.
            {
                livekit_failure_reason_t r = livekit_room_get_failure_reason(room_handle);
                ESP_LOGE(TAG, "Failure reason: %s", livekit_failure_reason_str(r));
                ui_connection_failed(describe_livekit_failure(r));
            }
            return;  // skip the duplicate failure-reason log below
    }

    livekit_failure_reason_t reason = livekit_room_get_failure_reason(room_handle);
    if (reason != LIVEKIT_FAILURE_REASON_NONE)
    {
        ESP_LOGE(TAG, "Failure reason: %s", livekit_failure_reason_str(reason));
    }
}

/// Invoked when participant information is received.
static void on_participant_info(const livekit_participant_info_t *info, void *ctx)
{
    if (info->kind != LIVEKIT_PARTICIPANT_KIND_AGENT)
    {
        // Only handle agent participants for this example.
        return;
    }
    bool joined = false;
    switch (info->state)
    {
        case LIVEKIT_PARTICIPANT_STATE_ACTIVE:
            joined = true;
            break;
        case LIVEKIT_PARTICIPANT_STATE_DISCONNECTED:
            joined = false;
            break;
        default:
            return;
    }
    if (joined != agent_joined)
    {
        ESP_LOGI(TAG, "Agent has %s the room", joined ? "joined" : "left");
        agent_joined = joined;
        if (joined) {
            // Agent showed up before the watchdog fired — cancel the timer
            // so we don't show "Agent unavailable" on a healthy session.
            cancel_agent_join_timer();
        }
    }
}

/// Invoked when the remote participant publishes a data packet.
///
/// Used here for half-duplex echo suppression: the agent.py side publishes
/// `"speaking"` / `"listening"` strings on topic `"agent_state"` whenever
/// its `agent_state_changed` event fires. We toggle the mic mute at the
/// codec accordingly so the speaker output doesn't loop back through the
/// publish track. See media_set_mic_muted() for details.
///
/// Other topics are ignored — the frontend's "transcription" / "artifact"
/// topics are handled by the web client, not this firmware.
static void on_data_received(const livekit_data_received_t *data, void *ctx)
{
    if (data == NULL || data->topic == NULL || data->payload.bytes == NULL) {
        return;
    }

    if (strcmp(data->topic, "agent_state") == 0) {
        // Payload is a short ASCII string ("speaking" / "listening" / "thinking" / "idle").
        // We only mute on the literal "speaking" transition — every other state
        // (listening / thinking / idle) means the agent isn't producing audio
        // so the mic should be live to capture the user. In meeting mode the
        // transcriber never speaks, so this leaves the mic live throughout.
        const uint8_t *p = data->payload.bytes;
        size_t n = data->payload.size;
        bool speaking = (n == 8 && memcmp(p, "speaking", 8) == 0);
        media_set_mic_muted(speaking);
        return;
    }

    // Topics parsed as JSON: "transcription" carries voice-mode artifacts (the
    // plan-of-day picker) AND meeting transcript lines; "coach" carries meeting
    // coach cards. Anything else is ignored. NOTE: this is no longer gated on
    // s_meeting_mode up front — the plan picker is a VOICE-mode feature.
    bool is_transcript = (strcmp(data->topic, "transcription") == 0);
    bool is_coach      = (strcmp(data->topic, "coach") == 0);
    if (!is_transcript && !is_coach) {
        return;
    }

    // Copy payload to a bounded, null-terminated heap buffer for cJSON. The
    // plan artifact (scheduledBlocks + summary) is larger than a transcript
    // line, so the cap is generous; oversized payloads truncate at a UTF-8
    // boundary (a mangled tail just fails cJSON_Parse → no-op, no crash).
    size_t n = data->payload.size;
    if (n == 0) return;
    if (n > 8192) {
        n = 8192;
        const uint8_t *pb = (const uint8_t *)data->payload.bytes;
        while (n > 0 && (pb[n] & 0xC0) == 0x80) n--;
    }
    char *buf = (char *)malloc(n + 1);
    if (buf == NULL) return;
    memcpy(buf, data->payload.bytes, n);
    buf[n] = '\0';

    cJSON *obj = cJSON_Parse(buf);
    free(buf);
    if (obj == NULL) {
        return;
    }
    cJSON *type = cJSON_GetObjectItem(obj, "type");
    const char *type_str = (type && cJSON_IsString(type)) ? type->valuestring : "";

    // Voice-mode plan-of-day picker: "show_artifact" envelope with artifact_kind
    // "voice-plan-day" (agent plan_my_day → publish_artifact). Render the
    // proposed focus blocks as a knob-selectable list. Works in voice mode.
    if (is_transcript && strcmp(type_str, "show_artifact") == 0) {
        cJSON *kind = cJSON_GetObjectItem(obj, "artifact_kind");
        const char *kind_str = (kind && cJSON_IsString(kind)) ? kind->valuestring : "";
        if (strcmp(kind_str, "voice-plan-day") == 0) {
            cJSON *adata  = cJSON_GetObjectItem(obj, "artifact_data");
            cJSON *blocks = adata ? cJSON_GetObjectItem(adata, "scheduledBlocks") : NULL;
            if (blocks && cJSON_IsArray(blocks)) {
                ui_plan_block_t rows[UI_PLAN_MAX_BLOCKS];
                int rc = 0;
                cJSON *b = NULL;
                cJSON_ArrayForEach(b, blocks) {
                    if (rc >= UI_PLAN_MAX_BLOCKS) break;
                    cJSON *jid    = cJSON_GetObjectItem(b, "id");
                    cJSON *jlabel = cJSON_GetObjectItem(b, "startLabel");
                    cJSON *jtitle = cJSON_GetObjectItem(b, "taskSummary");
                    cJSON *jbreak = cJSON_GetObjectItem(b, "isBreak");
                    // Skip break rows — only bookable focus/meeting blocks.
                    if (jbreak && cJSON_IsBool(jbreak) && cJSON_IsTrue(jbreak)) continue;
                    const char *idv = (jid && cJSON_IsString(jid)) ? jid->valuestring : "";
                    if (idv[0] == '\0') continue;  // need an id to round-trip
                    rows[rc].id    = idv;
                    rows[rc].label = (jlabel && cJSON_IsString(jlabel)) ? jlabel->valuestring : "";
                    rows[rc].title = (jtitle && cJSON_IsString(jtitle)) ? jtitle->valuestring : "";
                    rc++;
                }
                if (rc > 0) {
                    ui_plan_show(rows, rc);  // strings valid until cJSON_Delete below
                }
            }
        }
        cJSON_Delete(obj);
        return;
    }

    // Meeting-mode data channels (only while a silent meeting is active).
    if (s_meeting_mode) {
        if (is_transcript && strcmp(type_str, "meeting_transcript") == 0) {
            cJSON *text = cJSON_GetObjectItem(obj, "text");
            if (text && cJSON_IsString(text) && strlen(text->valuestring) > 0) {
                ui_meeting_transcript_line(text->valuestring);
            }
        } else if (is_coach && strcmp(type_str, "coach_suggestion") == 0) {
            cJSON *display = cJSON_GetObjectItem(obj, "display_text");
            if (display && cJSON_IsString(display) && strlen(display->valuestring) > 0) {
                ui_meeting_coach_card(display->valuestring);
            }
        }
    }

    cJSON_Delete(obj);
}

// Publish the user's knob-selected plan block id on the "plan_select" data
// topic. The agent (agent.py on_data_received) books that block and speaks a
// confirmation. Called from button_task on a short knob press while the plan
// picker is showing (see main.c).
void plan_confirm_selection(void)
{
    const char *id = ui_plan_get_selected_id();
    ui_plan_hide();
    if (id == NULL || id[0] == '\0' || room_handle == NULL) {
        return;
    }
    char json[96];
    int len = snprintf(json, sizeof(json), "{\"selected_block_id\":\"%s\"}", id);
    if (len <= 0 || len >= (int)sizeof(json)) {
        return;
    }
    livekit_data_payload_t payload = {
        .bytes = (uint8_t *)json,
        .size  = (size_t)len,
    };
    livekit_data_publish_options_t options = {
        .payload = &payload,
        .topic   = "plan_select",
        .lossy   = false,
        .destination_identities = NULL,
        .destination_identities_count = 0,
    };
    livekit_err_t err = livekit_room_publish_data(room_handle, &options);
    ESP_LOGI(TAG, "plan_select published (id=%s) err=%d", id, (int)err);
}

/// Invoked by a remote participant to set the state of an on-board LED.
static void set_led_state(const livekit_rpc_invocation_t *invocation, void *ctx)
{
    // LED control is currently disabled in this simplified firmware
    // as it requires specific BSP dependencies that are not included.
    // Use Watcher-specific LED code if needed later.
    if (invocation->payload == NULL)
    {
        livekit_rpc_return_error("Missing payload");
        return;
    }
    livekit_rpc_return_ok(NULL);
}

/// Invoked by a remote participant to get the current CPU temperature.
static void get_cpu_temp(const livekit_rpc_invocation_t *invocation, void *ctx)
{
    float temp = board_get_temp();
    char temp_string[16];
    snprintf(temp_string, sizeof(temp_string), "%.2f", temp);
    livekit_rpc_return_ok(temp_string);
}

static void connect_room_internal(bool meeting)
{
    // Show "Connecting..." immediately. The HTTP credentials fetch below
    // takes ~1–3 s, and LiveKit's CONNECTING state callback only fires
    // AFTER that. Without this call the user sees nothing during the
    // whole roundtrip and thinks the knob press did nothing.
    ui_wifi_connecting();

    // Disable WiFi power-save for the duration of the voice session.
    // The IDF default (WIFI_PS_MIN_MODEM) sleeps the radio in ~307 ms
    // windows, causing audio Opus packets to arrive in bursts. Even with
    // a 340 ms render FIFO (see media.c), tail-of-burst can graze the
    // buffer and produce small audible artifacts. WIFI_PS_NONE keeps
    // the radio awake so audio packets flow smoothly. Re-enabled in
    // leave_room() so idle battery isn't impacted.
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(NONE) failed: %s", esp_err_to_name(ps_err));
    } else {
        ESP_LOGI(TAG, "WiFi power-save disabled for session");
    }

    // Reset the agent-join failure flag for this attempt. If a stale room
    // handle exists from a prior agent-timeout, leave it (the block below
    // sees it's CONNECTED but flagged failed and tears it down).
    bool had_agent_failure = s_agent_join_failed;
    s_agent_join_failed = false;
    agent_joined = false;
    s_reconnecting = false;
    cancel_reconnect_timer();  // clear any stuck-reconnect watchdog from a prior wedge

    // If a previous attempt left a stale handle in FAILED/DISCONNECTED state,
    // tear it down so we can re-create cleanly. (room_is_active() now ignores
    // FAILED handles, so handle_single_click reached us — but we still need
    // to release the LiveKit-side resources.) Same for an agent-join failure
    // where the room is technically CONNECTED but the agent never showed.
    if (room_handle != NULL)
    {
        if (had_agent_failure)
        {
            ESP_LOGI(TAG, "Cleaning up agent-less room before retry");
            leave_room();
        }
        else
        {
            livekit_connection_state_t st = livekit_room_get_state(room_handle);
            if (st == LIVEKIT_CONNECTION_STATE_FAILED || st == LIVEKIT_CONNECTION_STATE_DISCONNECTED)
            {
                ESP_LOGI(TAG, "Cleaning up stale room handle (state=%s) before retry",
                         livekit_connection_state_str(st));
                leave_room();  // destroys room_handle, sets it to NULL
            }
            else
            {
                ESP_LOGE(TAG, "Room already created (state=%s)", livekit_connection_state_str(st));
                return;
            }
        }
    }

    // Latch the session mode AFTER any stale-handle cleanup above (the cleanup
    // path calls leave_room(), which resets s_meeting_mode). Tell the backend
    // which mode to connect in BEFORE the credentials fetch below.
    s_meeting_mode = meeting;
    aligned_set_connect_mode(meeting ? "meeting" : "voice");

    // Reinitialize media if it was cleaned up (e.g., after leaving a room)
    if (media_get_capturer() == NULL || media_get_renderer() == NULL)
    {
        ESP_LOGI(TAG, "Reinitializing media systems...");
        media_init();
    }
    else
    {
        // RECONNECT: fully re-init BOTH capturer and renderer — not just the
        // capturer.
        //
        // The Watcher's I2S is DUPLEX (shared RX+TX on one port). On ESP32-S3 the
        // RX channel cannot be cleanly disabled/re-enabled while TX stays open —
        // the codec data-if defers it ("pending in channel for out channel
        // running", audio_codec_data_i2s.c). Tearing down ONLY the capturer (RX)
        // while the renderer (TX) stayed alive left RX in that half-cycled state,
        // so the first i2s read after the codec reopen timed out → the esp_capture
        // audio thread exited → dead mic on ~1/3 of reconnects (the famous
        // "AUD_SRC ret -8"; first-connect-after-boot always worked).
        //
        // The ORIGINAL firmware (commit 19a950f) tore BOTH down together via
        // media_cleanup() on disconnect and reconnect always worked. That was
        // moved off the DISCONNECT path because media_cleanup() races the still-
        // draining peer_task and crashes (see leave_room). Doing the full teardown
        // here at JOIN time is safe — the prior room was already destroyed by
        // leave_room (close → settle → destroy), so no peer_task references the
        // media — and it cycles the whole duplex I2S cleanly, exactly like a cold
        // boot.
        // ── Option A (init-once / keep-alive) ──────────────────────────────
        // Do NOT media_cleanup()+media_init() per connect. Tearing down and
        // rebuilding the shared full-duplex I2S on every connect is what made the
        // clock-start fragile: on a "dirty" entry state the rebuild leaves the RX
        // read timing out (i2s ret 0x107 → "AUD_SRC -8"), which kills BOTH the mic
        // AND the speaker because RX+TX share one I2S clock — the "first connect
        // doesn't work, reconnect does" bug. The LiveKit engine only start/stops the
        // DATA flow (esp_capture_start / esp_capture_stop, engine.c:284/304) and
        // never closes the capturer/renderer, so they are safe to REUSE across
        // sessions — exactly the boot state the mic reliably works in. This matches
        // the documented pattern (ESP-ADF keep_pipeline_alive; ESP-IDF requires a
        // shared duplex TX/RX pair to stay in the SAME start/stop state). We keep
        // only the mic RIGHT-slot restore below, which the prior session's codec
        // close can remap to the silent LEFT slot. (2026-06-18, replaces the
        // per-connect teardown band-aid that this churn used to be.)
        ESP_LOGI(TAG, "Reconnect: keep media alive (no teardown), restore mic slot only");
        // RIGHT-slot re-pin for the mic. After a disconnect, the LiveKit teardown
        // closed the record codec (input_opened=false), so on reconnect the
        // capturer's channel=1 codec open runs set_fmt and the I2S data-if
        // UNCONDITIONALLY remaps a mono open to slot 0 = LEFT = the unwired,
        // SILENT slot (audio_codec_data_i2s.c:414-419). board_codec_reinit_record()
        // fixes this at the I2S-peripheral level: it re-applies the boot slot
        // config (I2S_STD_SLOT_RIGHT) directly on the RX channel AND reopens the
        // record handle the boot way (channel=2 + MAKE_CHANNEL_MASK(1)) so
        // input_opened=true → the capturer's later channel=1 open hits the
        // "Input already open" no-op and can't remap the slot back to LEFT.
        //
        // Safe HERE (between media_init and livekit_room_connect) but NOT after
        // the renderer is live: the play/TX codec handle is still CLOSED at this
        // point (av_render defers its esp_codec_dev_open to the first agent audio
        // packet), so we never disable RX while TX is streaming → no "AUD_SRC
        // ret -8". The earlier reverted attempt called bsp_codec_set_fs AFTER the
        // renderer was up, which is what produced the -8. Setting the slot in the
        // esp_capture source channel_mask was ALSO tried and reverted: the
        // channel==1 branch in the data-if overwrites it (no-op), and forcing
        // channel=2 there desyncs esp_capture's enable tracking → an
        // "i2s_channel_disable: channel not enabled" reconnect loop. See
        // board_codec_reinit_record() for the full root-cause writeup.
        board_codec_reinit_record();
    }

    // Guard: if media init/reset failed, the capturer and/or renderer is NULL.
    // Creating a room with a NULL capturer publishes silence with no error —
    // a silent dead session. Bail cleanly so the user sees a failure and can
    // retry, rather than staring at a listening orb that can't hear them.
    if (media_get_capturer() == NULL || media_get_renderer() == NULL)
    {
        ESP_LOGE(TAG, "Media not ready after init/reset — aborting connect");
        ui_connection_failed("Audio init failed");
        return;
    }

    // Always start the session with mic LIVE. The half-duplex echo-suppression
    // path (on_data_received) will mute it during agent speech. If a prior
    // session ended mid-utterance ("speaking" sent, no "listening" follow-up
    // before close), the codec could still be muted — make sure we recover.
    media_set_mic_muted(false);

    livekit_room_options_t room_options
        = { .publish = { .kind = LIVEKIT_MEDIA_TYPE_AUDIO, .audio_encode = { .codec = LIVEKIT_AUDIO_CODEC_OPUS, .sample_rate = 48000, .channel_count = 1 }, .capturer = media_get_capturer() },
              .subscribe = { .kind = LIVEKIT_MEDIA_TYPE_AUDIO, .renderer = media_get_renderer() },
              .on_state_changed = on_state_changed,
              .on_participant_info = on_participant_info,
              .on_data_received = on_data_received };
    if (livekit_room_create(&room_handle, &room_options) != LIVEKIT_ERR_NONE)
    {
        ESP_LOGE(TAG, "Failed to create room");
        ui_connection_failed("Audio init failed");
        // Don't leave a partial/invalid handle behind. The next connect attempt
        // checks `room_handle != NULL` and would call livekit_room_get_state()
        // on a half-created handle and crash. Force a clean NULL so the retry
        // re-creates from scratch.
        room_handle = NULL;
        return;
    }

    // Register RPC handlers so they can be invoked by remote participants.
    livekit_room_rpc_register(room_handle, "set_led_state", set_led_state);
    livekit_room_rpc_register(room_handle, "get_cpu_temp", get_cpu_temp);

    // Get dynamic LiveKit credentials from Aligned backend
    ESP_LOGI(TAG, "Fetching LiveKit credentials from Aligned backend...");
    esp_err_t creds_err = aligned_get_livekit_credentials();
    if (creds_err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get LiveKit credentials from Aligned backend");
        ESP_LOGE(TAG, "Make sure device token is set and backend is reachable");
        ui_connection_failed(aligned_get_last_error_msg());
        leave_room();  // tear down the never-connected room handle
        return;
    }

    const char *livekit_url = aligned_get_livekit_url();
    const char *livekit_token = aligned_get_livekit_token();

    if (!livekit_url || strlen(livekit_url) == 0 || !livekit_token || strlen(livekit_token) == 0)
    {
        ESP_LOGE(TAG, "Invalid credentials received from backend");
        ui_connection_failed("Bad credentials");
        leave_room();
        return;
    }

    ESP_LOGI(TAG, "Connecting to LiveKit room: %s", aligned_get_room_name());
    ESP_LOGI(TAG, "Voice provider: %s ($0.05/min)", "xAI Grok Realtime");

    // Connect to LiveKit room using dynamic credentials
    livekit_err_t connect_res = livekit_room_connect(room_handle, livekit_url, livekit_token);

    if (connect_res != LIVEKIT_ERR_NONE)
    {
        ESP_LOGE(TAG, "Failed to connect to room (err=%d)", connect_res);
        ui_connection_failed("Connect failed");
        leave_room();
    }
    // On success, on_state_changed will fire with CONNECTING then CONNECTED.
}

void join_room()
{
    // Conversational voice session (the default knob single-click action).
    connect_room_internal(false);
}

void start_meeting()
{
    // Silent live-meeting transcription session (Live Meeting button).
    connect_room_internal(true);
}

void leave_room()
{
    if (room_handle == NULL)
    {
        ESP_LOGE(TAG, "Room not created");
        return;
    }

    /* Re-entry guard: prevent double leave_room() calls from crashing.
     * button_task (main.c) and console commands (cmd.c) run on separate tasks
     * and can race. The check-and-set MUST be atomic — a plain check-then-set
     * let both callers pass and double-close/destroy the same handle
     * (use-after-free). Take the OLD value under the lock; only the caller that
     * observed `false` proceeds. */
    bool already_leaving;
    taskENTER_CRITICAL(&s_leave_mux);
    already_leaving = s_leaving_room;
    s_leaving_room = true;
    taskEXIT_CRITICAL(&s_leave_mux);
    if (already_leaving)
    {
        ESP_LOGW(TAG, "Already leaving room, ignoring duplicate request");
        return;
    }

    livekit_room_handle_t handle = room_handle;
    room_handle = NULL;  /* Clear immediately to prevent re-entry via room_is_active() */

    // Cancel any in-flight watchdogs + reset the failure flags.
    // We're tearing down the room regardless of why; the next attempt starts fresh.
    cancel_agent_join_timer();
    cancel_reconnect_timer();
    s_reconnecting = false;
    s_agent_join_failed = false;
    agent_joined = false;

    // Update UI: voice chat is no longer active
    ui_set_voice_active(false);

    // Close LiveKit FIRST — this tells the server we're leaving and signals
    // the peer_task / publish_task to stop. Then poll until the room is in
    // DISCONNECTED state so we know the tasks have drained.
    //
    // We do NOT call media_cleanup() on a normal disconnect. The previous
    // ordering (media_cleanup → close) caused this exact panic on every
    // disconnect:
    //
    //   assert failed: spinlock_acquire spinlock.h:142 (lock->count == 0)
    //   peer_task → on_peer_sub_audio_frame → av_render_add_audio_data
    //     → media_lib_mutex_lock → freed spinlock → reboot
    //
    // Incoming audio packets kept arriving at peer_task after media_cleanup
    // destroyed the renderer's mutex. Reboot triggers full WiFi reconnect.
    //
    // Leaving the audio pipeline alive across sessions also makes reconnect
    // dramatically faster (no media_init on the next join_room).
    ESP_LOGI(TAG, "Closing LiveKit room...");
    livekit_err_t close_err = livekit_room_close(handle);
    if (close_err != LIVEKIT_ERR_NONE)
    {
        ESP_LOGW(TAG, "Failed to close room (err=%d), continuing with destroy...", close_err);
    }

    // Settle delay between close and destroy. close() signals the engine +
    // peer/GMF audio tasks to stop, but that teardown is ASYNC. Calling
    // destroy() immediately frees a task's context out from under a
    // publish-path GMF task that hasn't stopped yet — the audio-encoder task
    // (aenc_0) resumes after destroy freed it and jumps through a freed
    // callback pointer into an unmapped flash address:
    //
    //   Guru Meditation: Cache disabled but cached memory region accessed
    //   MMU invalid entry @ 0x43cb8fac  (verified 2026-05-22, meeting end)
    //
    // This is MEETING-mode-specific: the mic publishes continuously (the
    // silent transcriber sends no audio back, so the subscribe path the
    // 2026-05-18 fix addressed is idle, but the publish/encoder path is hot
    // and loses the close→destroy race). Voice mode mutes the mic while the
    // agent speaks, so its encoder is usually idle at teardown.
    //
    // We deliberately do NOT poll livekit_room_get_state() — on an
    // already-closed handle it returns DISCONNECTED immediately, so the old
    // poll loop fell straight through and destroyed too early (the 2026-05-18
    // engine_destroy LoadProhibited at engine.c:1181). A fixed delay forces
    // the wait the broken poll skipped. Teardown is typically ~200 ms; 500 ms
    // gives margin without a perceptible disconnect lag.
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "Destroying LiveKit room handle...");
    livekit_err_t destroy_err = livekit_room_destroy(handle);
    if (destroy_err != LIVEKIT_ERR_NONE)
    {
        ESP_LOGE(TAG, "Failed to destroy room (err=%d)", destroy_err);
    }

    agent_joined = false;
    s_leaving_room = false;
    s_meeting_mode = false;  // next session defaults to voice unless start_meeting() sets it

    // Restore WiFi power-save so the device doesn't burn battery while
    // idle waiting for the next knob press. Matches the IDF default that
    // join_room() turned off. Best-effort — if this fails the only cost
    // is slightly higher idle power.
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(MIN_MODEM) failed: %s", esp_err_to_name(ps_err));
    } else {
        ESP_LOGI(TAG, "WiFi power-save re-enabled (idle)");
    }

    ESP_LOGI(TAG, "Room disconnected successfully");
}

bool room_is_active(void)
{
    if (room_handle == NULL) return false;
    // A handle in FAILED/DISCONNECTED state still exists but is not usable —
    // treat it as inactive so handle_single_click() lets the user retry,
    // and join_room() will tear it down before re-creating.
    if (s_agent_join_failed) return false;  // agent never joined → effectively dead
    livekit_connection_state_t st = livekit_room_get_state(room_handle);
    return st == LIVEKIT_CONNECTION_STATE_CONNECTED ||
           st == LIVEKIT_CONNECTION_STATE_CONNECTING ||
           st == LIVEKIT_CONNECTION_STATE_RECONNECTING;
}

bool meeting_is_active(void)
{
    return s_meeting_mode && room_is_active();
}

void stop_meeting(void)
{
    if (!room_is_active())
    {
        return;
    }
    ESP_LOGI(TAG, "Ending live meeting");
    ui_meeting_end();   // hide transcript/coach UI, restore home wallpaper
    leave_room();       // server-side session close triggers transcript finalize
    // Brief pause so the end transition is visible, then ensure home chrome.
    vTaskDelay(pdMS_TO_TICKS(300));
    ui_show_wifi_button();
}

// Called from the LVGL touch callbacks — must be cheap and non-blocking.
void request_start_meeting(void) { s_req_start_meeting = true; }
void request_stop_meeting(void)  { s_req_stop_meeting = true; }

// Called from button_task (a normal FreeRTOS task) to perform the deferred,
// blocking start/stop work off the LVGL render task.
void service_meeting_requests(void)
{
    if (s_req_start_meeting)
    {
        s_req_start_meeting = false;
        if (!room_is_active())
        {
            ESP_LOGI(TAG, "Live Meeting button → starting meeting");
            start_meeting();
        }
    }
    if (s_req_stop_meeting)
    {
        s_req_stop_meeting = false;
        if (room_is_active())
        {
            ESP_LOGI(TAG, "End button → stopping meeting");
            stop_meeting();
        }
    }
}
