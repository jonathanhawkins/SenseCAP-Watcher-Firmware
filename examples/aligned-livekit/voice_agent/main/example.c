#include <string.h>
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
        case LIVEKIT_CONNECTION_STATE_RECONNECTING:
            ui_wifi_connecting();  // Show connecting state
            break;
        case LIVEKIT_CONNECTION_STATE_DISCONNECTED:
            cancel_agent_join_timer();
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

    // Meeting-mode data channels: transcript lines (topic "transcription",
    // type "meeting_transcript") and coach cards (topic "coach"). Ignored
    // outside a meeting so voice-mode "transcription" packets (memory tool
    // debug events) don't render anything.
    if (!s_meeting_mode) {
        return;
    }
    bool is_transcript = (strcmp(data->topic, "transcription") == 0);
    bool is_coach = (strcmp(data->topic, "coach") == 0);
    if (!is_transcript && !is_coach) {
        return;
    }

    // Copy payload to a bounded, null-terminated buffer for cJSON.
    size_t n = data->payload.size;
    if (n == 0) return;
    if (n > 768) {
        n = 768;  // cap — transcript lines / coach text are short
        // Back off to a UTF-8 codepoint boundary so we don't truncate mid-
        // sequence (would render a tofu glyph). Continuation bytes are 10xxxxxx (L1).
        const uint8_t *pb = (const uint8_t *)data->payload.bytes;
        while (n > 0 && (pb[n] & 0xC0) == 0x80) n--;
    }
    char buf[769];
    memcpy(buf, data->payload.bytes, n);
    buf[n] = '\0';

    cJSON *obj = cJSON_Parse(buf);
    if (obj == NULL) {
        return;
    }
    cJSON *type = cJSON_GetObjectItem(obj, "type");
    const char *type_str = (type && cJSON_IsString(type)) ? type->valuestring : "";

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

    cJSON_Delete(obj);
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
     * The button task and console commands can race. */
    if (s_leaving_room)
    {
        ESP_LOGW(TAG, "Already leaving room, ignoring duplicate request");
        return;
    }
    s_leaving_room = true;

    livekit_room_handle_t handle = room_handle;
    room_handle = NULL;  /* Clear immediately to prevent re-entry via room_is_active() */

    // Cancel any in-flight agent-join watchdog + reset the failure flag.
    // We're tearing down the room regardless of why; the next attempt starts fresh.
    cancel_agent_join_timer();
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

    // Match the upstream LiveKit example's leave_room exactly: close, then
    // destroy back-to-back, no polling. The earlier state-polling loop here
    // panicked engine_destroy() on 2026-05-18 — `livekit_room_get_state` on
    // an already-closed handle would return DISCONNECTED immediately, then
    // destroy hit half-freed engine internals (LoadProhibited, engine.c:1181).
    // Reference: components/livekit__livekit/examples/voice_agent/main/example.c
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
