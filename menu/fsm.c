#include "fsm.h"
#include "actions.h"
#include "menu.h"
#include "render.h"
#include "sysfs.h"
#include "config.h"

#include <stddef.h>
#include <stdio.h>

/* ── Global app state ──────────────────────────────────────────────────────── */
AppState app_state = STATE_IDLE;

/* ═══════════════════════════════════════════════════════════════════════════
 * Transition handlers
 *
 * Convention: every handler ends with render() so the screen is always
 * up to date after a state change. Handlers that don't need fb_fd cast it.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void th_menu_up(int _)
{
    (void)_;
    selection = (selection > 0) ? selection - 1 : current_menu->count - 1;
    render();
}

static void th_menu_down(int _)
{
    (void)_;
    selection = (selection < current_menu->count - 1) ? selection + 1 : 0;
    render();
}

static void th_menu_select(int _)
{
    (void)_;
    menu_select();
    render();
}

static void th_menu_back(int _)
{
    (void)_;
    menu_back();
    render();
}

static void th_open_menu(int fb_fd)
{
    set_blank(fb_fd, false);
    current_menu = &ROOT_MENU;
    selection    = 0;
    app_state    = STATE_MENU;
    render();
}

static void th_close_menu(int _)
{
    (void)_;
    action_exit_menu();
    render();
}

static void th_bri_up(int _)
{
    (void)_;
    set_brightness(current_brightness + 1);
    render();
}

static void th_bri_down(int _)
{
    (void)_;
    set_brightness(current_brightness - 1);
    render();
}

static void th_bri_exit(int _)
{
    (void)_;
    app_state = STATE_MENU;
    render();
}

static void th_bat_dismiss(int _)
{
    (void)_;
    app_state = STATE_MENU;
    render();
}

static void th_idle_power(int fb_fd)
{
    set_blank(fb_fd, !screen_blanked);
}

static void th_reboot(int _)
{
    (void)_;
    char *const a[] = { "reboot", NULL };
    run_cmd((char *const *)a);
}

static void th_poweroff(int _)
{
    (void)_;
    char *const a[] = { "poweroff", NULL };
    run_cmd((char *const *)a);
}

static void th_home(int _)
{
    (void)_;
    action_fbkeyboard_toggle();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Transition table
 *
 * { current_state, event, handler }
 * STATE_COUNT = wildcard, matches any state.
 * First match wins — specific states must appear before wildcards.
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef void (*TransitionFn)(int fb_fd);

typedef struct {
    AppState     state;
    FsmEvent     event;
    TransitionFn handler;
} Transition;

static const Transition TRANSITIONS[] = {
    /* Brightness overlay */
    { STATE_BRIGHTNESS,   EVT_VOL_UP,            th_bri_up      },
    { STATE_BRIGHTNESS,   EVT_VOL_DOWN,          th_bri_down    },
    { STATE_BRIGHTNESS,   EVT_POWER_SHORT,       th_bri_exit    },
    { STATE_BRIGHTNESS,   EVT_BACK_KEY,          th_bri_exit    },

    /* Battery info — any key dismisses */
    { STATE_BATTERY_INFO, EVT_POWER_SHORT,       th_bat_dismiss },
    { STATE_BATTERY_INFO, EVT_VOL_UP,            th_bat_dismiss },
    { STATE_BATTERY_INFO, EVT_VOL_DOWN,          th_bat_dismiss },
    { STATE_BATTERY_INFO, EVT_BACK_KEY,          th_bat_dismiss },
    { STATE_BATTERY_INFO, EVT_MENU_KEY,          th_bat_dismiss },

    /* Menu navigation */
    { STATE_MENU,         EVT_VOL_UP,            th_menu_up     },
    { STATE_MENU,         EVT_VOL_DOWN,          th_menu_down   },
    { STATE_MENU,         EVT_POWER_SHORT,       th_menu_select },
    { STATE_MENU,         EVT_BACK_KEY,          th_menu_back   },
    { STATE_MENU,         EVT_MENU_KEY,          th_close_menu  },

    /* Idle */
    { STATE_IDLE,         EVT_POWER_SHORT,       th_idle_power  },

    /* Global — STATE_COUNT matches any state */
    { STATE_COUNT,        EVT_MENU_KEY,          th_open_menu   },
    { STATE_COUNT,        EVT_HOME_KEY,          th_home        },
    { STATE_COUNT,        EVT_POWER_LONG_REBOOT, th_reboot      },
    { STATE_COUNT,        EVT_POWER_LONG_OFF,    th_poweroff    },
};

#define TRANSITION_COUNT (sizeof(TRANSITIONS) / sizeof(TRANSITIONS[0]))

/* ── Dispatcher ────────────────────────────────────────────────────────────── */
void fsm_dispatch(FsmEvent evt, int fb_fd)
{
    for (size_t i = 0; i < TRANSITION_COUNT; i++) {
        const Transition *t = &TRANSITIONS[i];
        if ((t->state == STATE_COUNT || t->state == app_state) && t->event == evt) {
            t->handler(fb_fd);
            return;
        }
    }
    /* No matching transition — silently ignored by design */
}
