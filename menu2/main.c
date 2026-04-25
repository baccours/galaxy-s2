/*
 * main.c — FSM transition table, transition handlers, input translation,
 *           cleanup, and main().
 *
 * This file wires everything together.  It knows about:
 *   - the FSM (states, events, transition table)
 *   - input devices (poll loop, event → FsmEvent mapping)
 *   - process lifecycle (main, cleanup, signals)
 *
 * It does NOT know about menu content or rendering (menu.c) or
 * how system calls are made (system.c).
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -std=c11 -o menu main.c menu.c system.c
 */

#include "main.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * FSM — Transition handlers
 *
 * Each handler mutates global state (g_state, g_menu, g_sel, …) then
 * calls render() so the screen is always consistent after any transition.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void th_menu_up(void)
{
    g_sel = (g_sel > 0) ? g_sel - 1 : g_menu->count - 1;
    render();
}

static void th_menu_down(void)
{
    g_sel = (g_sel < g_menu->count - 1) ? g_sel + 1 : 0;
    render();
}

static void th_menu_select(void)
{
    const MenuItem *item = &g_menu->items[g_sel];
    if (item->submenu) {
        g_menu = item->submenu;
        g_sel  = 0;
        render();
    } else if (item->action) {
        item->action();
        /* action may have changed g_state (e.g. th_close_menu);
         * only re-render if still in a menu state. */
        if (g_state == STATE_MENU || g_state == STATE_BRIGHTNESS)
            render();
    }
}

/* th_close_menu is declared in main.h so menu.c's action_exit_menu can
 * call it without duplicating the state-transition logic. */
void th_close_menu(void)
{
    g_menu  = &g_root_menu;
    g_sel   = 0;
    g_state = STATE_IDLE;
    update_grabs();
    terminal_restore();
    fputs(T_CLEAR T_SHOW, stdout);
    fflush(stdout);
    /* render() intentionally not called here: screen is clear,
     * terminal is cooked, cursor is visible — nothing more to draw. */
}

static void th_menu_back(void)
{
    if (g_menu->parent) {
        g_menu = g_menu->parent;
        g_sel  = 0;
        render();
    } else {
        th_close_menu();   /* at root — BACK exits the menu */
    }
}

static void th_open_menu(void)
{
    if (g_screen_blank) return;           /* no menu on a blank screen */
    g_menu  = &g_root_menu;
    g_sel   = 0;
    g_state = STATE_MENU;
    update_grabs();
    terminal_raw();                        /* disable echo/canon while navigating */
    fputs(T_HIDE, stdout);
    render();
}

static void th_idle_power(void)
{
    fb_set_blank(!g_screen_blank);
    update_grabs();
}

static void th_home(void)
{
    /* HOME ignored in menu and on blank screen */
    if (g_state == STATE_MENU || g_screen_blank) return;
    fbkbd_set(!g_fbkbd_on);
}

/* Brightness overlay handlers */
void th_brightness_enter(void)
{
    g_brightness = brightness_read();  /* sync with real hw value */
    g_state = STATE_BRIGHTNESS;
    render();
}

static void th_brightness_up(void)
{
    brightness_write(g_brightness + 1);
    render();
}

static void th_brightness_down(void)
{
    brightness_write(g_brightness - 1);
    render();
}

static void th_brightness_exit(void)
{
    g_state = STATE_MENU;
    render();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * FSM — Declarative transition table
 *
 *   { state, event, handler }
 *   STATE_ANY = wildcard — matches any current state.
 *   First match wins: specific states must appear before STATE_ANY rows.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef void (*TransitionFn)(void);

typedef struct {
    AppState     state;
    FsmEvent     event;
    TransitionFn handler;
} Transition;

static const Transition TRANSITIONS[] = {
    /* ── Menu navigation ─────────────────────────────────────────────── */
    { STATE_MENU, EVT_VOL_UP,    th_menu_up     },
    { STATE_MENU, EVT_VOL_DOWN,  th_menu_down   },
    { STATE_MENU, EVT_POWER,     th_menu_select },
    { STATE_MENU, EVT_BACK_KEY,  th_menu_back   },
    { STATE_MENU, EVT_MENU_KEY,  th_close_menu  },

    /* ── Brightness overlay ──────────────────────────────────────────── */
    { STATE_BRIGHTNESS, EVT_VOL_UP,   th_brightness_up   },
    { STATE_BRIGHTNESS, EVT_VOL_DOWN, th_brightness_down },
    { STATE_BRIGHTNESS, EVT_POWER,    th_brightness_exit },
    { STATE_BRIGHTNESS, EVT_BACK_KEY, th_brightness_exit },

    /* ── Idle-specific ────────────────────────────────────────────────── */
    { STATE_IDLE, EVT_POWER,     th_idle_power  },

    /* ── Global (STATE_ANY after all specific rows) ───────────────────── */
    { STATE_ANY,  EVT_MENU_KEY,  th_open_menu   },
    { STATE_ANY,  EVT_HOME_KEY,  th_home        },
    { STATE_ANY,  EVT_POWER,     th_idle_power  }, /* unblank from blank  */
};
#define TRANSITION_COUNT (sizeof(TRANSITIONS) / sizeof(TRANSITIONS[0]))

static void fsm_dispatch(FsmEvent evt)
{
    for (size_t i = 0; i < TRANSITION_COUNT; i++) {
        const Transition *t = &TRANSITIONS[i];
        if ((t->state == STATE_ANY || t->state == g_state) && t->event == evt) {
            t->handler();
            return;   /* first match wins */
        }
    }
    /* Unhandled event in this state — silently ignored by design */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Input event → FSM event translation
 * ═══════════════════════════════════════════════════════════════════════════ */

static void process_ev(const struct input_event *ev)
{
    if (ev->type != EV_KEY) return;
    if (ev->value != 1)     return;   /* key-down only; ignore repeat & up */

    switch (ev->code) {
        case KEY_VOLUMEUP_CODE:   fsm_dispatch(EVT_VOL_UP);   break;
        case KEY_VOLUMEDOWN_CODE: fsm_dispatch(EVT_VOL_DOWN); break;
        case KEY_POWER_CODE:      fsm_dispatch(EVT_POWER);    break;
        case KEY_HOME_CODE:       fsm_dispatch(EVT_HOME_KEY); break;
        case KEY_MENU_CODE:       fsm_dispatch(EVT_MENU_KEY); break;
        case KEY_BACK_CODE:       fsm_dispatch(EVT_BACK_KEY); break;
        default: break;
    }
}

/* Drain all pending events from one fd into the FSM */
static void drain(int fd)
{
    struct input_event buf[32];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        int cnt = (int)(n / (ssize_t)sizeof(struct input_event));
        for (int i = 0; i < cnt; i++)
            process_ev(&buf[i]);
    }
    if (n < 0 && errno != EAGAIN && errno != EINTR)
        log_err("read event");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Cleanup & signals
 * ═══════════════════════════════════════════════════════════════════════════ */

static void cleanup(void)
{
    /* 1. Restore terminal first — most critical for the user's session.
     *    Only emit ANSI resets if we were actually in raw mode; otherwise
     *    the terminal was never touched and we leave it exactly as found. */
    if (terminal_restore()) {
        fputs(T_CLEAR T_RESET T_SHOW, stdout);
        fflush(stdout);
    }

    /* 2. Release input grabs — device usable again immediately.
     * gpio and touchkey were grabbed at startup; always release them.
     * Touch is managed via update_grabs() — release only if grabbed. */
    grab(g_fd_gpio,     false);
    grab(g_fd_touchkey, false);
    g_state = STATE_IDLE;   /* force update_grabs to release touch */
    g_screen_blank = false;
    update_grabs();         /* releases touch if it was grabbed */

    /* 3. Close file descriptors. */
    if (g_fd_gpio     >= 0) close(g_fd_gpio);
    if (g_fd_touchkey >= 0) close(g_fd_touchkey);
    if (g_fd_touch    >= 0) close(g_fd_touch);

    if (g_fd_fb >= 0) {
        fb_set_blank(false);
        close(g_fd_fb);
    }

    log_info("done");
}

static void sig_handler(int s) { (void)s; g_running = 0; }

/* ═══════════════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    /* Signals */
    struct sigaction sa = { .sa_handler = sig_handler };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    sigaction(SIGCHLD, &(struct sigaction){ .sa_handler = SIG_DFL }, NULL);

    /* Link submenu parent pointers — cannot be done in static initialisers */
    g_net_menu.parent = &g_root_menu;
    g_pwr_menu.parent = &g_root_menu;

    /* Open fb_blank once; reused for every screen-blank write */
    g_fd_fb = open(FB_BLANK_PATH, O_WRONLY | O_CLOEXEC);
    if (g_fd_fb < 0) { log_err("open " FB_BLANK_PATH); return EXIT_FAILURE; }

    /* Button devices: grabbed permanently (sole consumer).
     * Touchscreen: opened ungrabbed; grab managed by update_grabs(). */
    g_fd_gpio     = open_dev(DEV_GPIO,     true);
    g_fd_touchkey = open_dev(DEV_TOUCHKEY, true);
    g_fd_touch    = open_dev(DEV_TOUCH,    false);

    if (g_fd_gpio < 0 || g_fd_touchkey < 0) {
        log_info("Cannot open required button devices — aborting.");
        cleanup();
        return EXIT_FAILURE;
    }

    /* poll(2) on 2 fds — appropriate for this device count */
    struct pollfd pfds[2] = {
        { .fd = g_fd_gpio,     .events = POLLIN },
        { .fd = g_fd_touchkey, .events = POLLIN },
    };

    g_menu = &g_root_menu;

    /* Terminal stays in normal (cooked) mode at startup.
     * terminal_raw() is called only when the menu opens (th_open_menu)
     * and terminal_restore() is called when it closes (th_close_menu).
     * This lets the user type normally in the terminal when the menu is off. */
    log_info("started — MENU button opens/closes menu");

    while (g_running) {
        int n = poll(pfds, 2, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            log_err("poll");
            break;
        }
        if (pfds[0].revents & POLLIN) drain(g_fd_gpio);
        if (pfds[1].revents & POLLIN) drain(g_fd_touchkey);
    }

    cleanup();
    return EXIT_SUCCESS;
}
