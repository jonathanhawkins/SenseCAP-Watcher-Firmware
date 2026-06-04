
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void join_room();
void leave_room();
bool room_is_active(void);

// Live-meeting (silent transcription) session lifecycle.
void start_meeting();       // begin a Live Meeting (connects with mode="meeting")
void stop_meeting(void);    // end the meeting, finalize server-side, return home
bool meeting_is_active(void);

// Touch-button → worker-task handoff (see example.c). UI callbacks call the
// request_* setters (cheap, non-blocking); button_task calls
// service_meeting_requests() to perform the blocking start/stop off the LVGL task.
void request_start_meeting(void);
void request_stop_meeting(void);
void service_meeting_requests(void);

// Plan-of-day picker: publish the knob-selected block id on the "plan_select"
// data topic so the agent books it. Called from button_task on a short press
// while ui_plan_is_active(). Hides the picker.
void plan_confirm_selection(void);

#ifdef __cplusplus
}
#endif
