
#pragma once

#include <stdbool.h>
#include "esp_capture.h"
#include "av_render.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Initializes the capturer and renderer systems.
int media_init(void);

/// Returns the capturer handle.
///
/// This handle is provided to a LiveKit room when initialized to enable
/// publishing tracks from captured media (i.e. audio from a microphone and/or
/// video from a camera).
///
/// How the capturer is configured is determined by the requirements of
/// your application and the hardware you are using.
///
esp_capture_handle_t media_get_capturer(void);

/// Returns the renderer handle.
///
/// This handle is provided to a LiveKit room when initialized to enable
/// rendering media from subscribed tracks (i.e. playing audio through a
/// speaker and/or displaying video to a screen).
///
/// How the renderer is configured is determined by the requirements of
/// your application and the hardware you are using.
///
av_render_handle_t media_get_renderer(void);

/// Cleans up media systems (capture and render).
/// Call this before closing a LiveKit room to prevent hangs.
void media_cleanup(void);

/// Rebuild ONLY the capturer (close + reopen) for a fresh audio-source thread.
///
/// The reused esp_capture audio source intermittently fails its first read after
/// a stop→start cycle on the 2nd+ session ("AUD_SRC: Failed to read audio frame
/// ret -8") → the capture thread exits → dead mic → choppy/deaf on RECONNECT
/// (first-connect-after-boot always works because media_init builds it fresh).
/// Call at the top of join_room on a reconnect (capturer already exists), BEFORE
/// livekit_room_create starts it. Safe there: the prior room was destroyed by
/// leave_room, so no peer_task references the capture (unlike media_cleanup during
/// a live disconnect, which crashes — see .claude/rules/watcher-livekit-teardown.md).
void media_reset_capturer(void);

/// Mute (or unmute) the microphone at the codec hardware.
///
/// Used for half-duplex echo suppression: while the agent is speaking, we
/// mute the mic so the speaker output doesn't loop back through the
/// publish track. Without an AEC source (see note in media.c), this is
/// how we keep the agent from hearing itself.
///
/// Safe to call before media_init() — it just no-ops if the codec isn't
/// open yet. Idempotent.
void media_set_mic_muted(bool muted);

#ifdef __cplusplus
}
#endif

