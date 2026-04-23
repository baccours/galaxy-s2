#ifndef FSM_H
#define FSM_H

/* ── States ────────────────────────────────────────────────────────────────── */
typedef enum {
    STATE_IDLE,         /* Screen on, no menu                    */
    STATE_MENU,         /* Menu visible and navigable            */
    STATE_BRIGHTNESS,   /* Brightness adjustment overlay         */
    STATE_BATTERY_INFO, /* Battery reading shown, waiting for key*/
    STATE_COUNT         /* Sentinel — used as wildcard           */
} AppState;

/* ── Events ────────────────────────────────────────────────────────────────── */
typedef enum {
    EVT_VOL_UP,
    EVT_VOL_DOWN,
    EVT_POWER_SHORT,
    EVT_POWER_LONG_REBOOT,
    EVT_POWER_LONG_OFF,
    EVT_MENU_KEY,
    EVT_BACK_KEY,
    EVT_HOME_KEY,
} FsmEvent;

/* ── Global app state (written by actions/handlers, read by render) ────────── */
extern AppState app_state;

/* ── Dispatch ──────────────────────────────────────────────────────────────── */
void fsm_dispatch(FsmEvent evt, int fb_fd);

#endif /* FSM_H */
