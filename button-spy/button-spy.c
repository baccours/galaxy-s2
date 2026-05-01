/*
 * button-spy.c — FSM transition table, transition handlers, input translation,
 *           cleanup, and main().
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -std=c11 -o button-spy button-spy.c menu.c system.c
 */

#include "button-spy.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ── FSM transition handlers ─────────────────────────────────────────────── */

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
        net_invalidate();
        render();
    } else if (item->action) {
        item->action();
        if (g_state == STATE_MENU || g_state == STATE_BRIGHTNESS)
            render();
    }
}

void th_close_menu(void)
{
    g_menu  = &g_root_menu;
    g_sel   = 0;
    g_state = STATE_IDLE;
    touch_inhibit(false);
    terminal_restore();
    fputs(T_CLEAR T_SHOW, g_tty);
    fflush(g_tty);
}

static void th_menu_back(void)
{
    if (g_menu->parent) {
        g_menu = g_menu->parent;
        g_sel  = 0;
        net_invalidate();
        render();
    } else {
        th_close_menu();
    }
}

static void th_open_menu(void)
{
    if (g_screen_blank) return;
    g_menu  = &g_root_menu;
    g_sel   = 0;
    g_state = STATE_MENU;
    touch_inhibit(true);
    terminal_raw();
    fputs(T_HIDE, g_tty);
    render();
}

static void th_idle_power(void)
{
    fb_set_blank(!g_screen_blank);
    touch_inhibit(g_screen_blank);
}

static void th_home(void)
{
    if (g_state == STATE_MENU || g_screen_blank) return;
    fbkbd_set(!g_fbkbd_on);
}

void th_brightness_enter(void)
{
    g_brightness = brightness_read();
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

/* ── FSM declarative transition table ────────────────────────────────────────
 * { state, key_code, handler } — STATE_ANY matches any state.
 * First match wins: specific states must appear before STATE_ANY rows.
 */
typedef void (*TransitionFn)(void);

typedef struct {
    AppState     state;
    unsigned int key_code;
    TransitionFn handler;
} Transition;

static const Transition TRANSITIONS[] = {
    { STATE_MENU,       KEY_VOLUMEUP,   th_menu_up         },
    { STATE_MENU,       KEY_VOLUMEDOWN, th_menu_down       },
    { STATE_MENU,       KEY_POWER,      th_menu_select     },
    { STATE_MENU,       KEY_BACK,       th_menu_back       },
    { STATE_MENU,       KEY_MENU,       th_close_menu      },

    { STATE_BRIGHTNESS, KEY_VOLUMEUP,   th_brightness_up   },
    { STATE_BRIGHTNESS, KEY_VOLUMEDOWN, th_brightness_down },
    { STATE_BRIGHTNESS, KEY_POWER,      th_brightness_exit },
    { STATE_BRIGHTNESS, KEY_BACK,       th_brightness_exit },

    { STATE_IDLE,       KEY_POWER,      th_idle_power      },

    { STATE_ANY,        KEY_MENU,       th_open_menu       },
    { STATE_ANY,        KEY_OK,         th_home            },
    { STATE_ANY,        KEY_POWER,      th_idle_power      },
};
#define TRANSITION_COUNT ARRAY_SIZE(TRANSITIONS)

static void drain(int fd)
{
    struct input_event buf[32];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        int cnt = (int)(n / (ssize_t)sizeof(struct input_event));
        for (int i = 0; i < cnt; i++) {
            const struct input_event *ev = &buf[i];
            if (ev->type != EV_KEY || ev->value != 1) continue;
            for (size_t j = 0; j < TRANSITION_COUNT; j++) {
                const Transition *t = &TRANSITIONS[j];
                if (t->key_code == ev->code &&
                    (t->state == STATE_ANY || t->state == g_state)) {
                    t->handler();
                    break;
                }
            }
        }
    }
    if (n < 0 && errno != EAGAIN && errno != EINTR)
        log_err("read event");
}

/* ── Cleanup & signals ────────────────────────────────────────────────────── */

static void cleanup(void)
{
    if (g_tty) {
        fputs(T_CLEAR T_RESET T_SHOW, g_tty);
        fflush(g_tty);
    }
    terminal_close();

    /* Release touchscreen inhibit before exit */
    touch_inhibit(false);

    /* Explicitly release grabs so other processes can use the devices
     * immediately — do not rely on the kernel releasing on fd close. */
    if (g_fd_gpio     >= 0) { release_button_dev(g_fd_gpio);     close(g_fd_gpio); }
    if (g_fd_touchkey >= 0) { release_button_dev(g_fd_touchkey); close(g_fd_touchkey); }

    if (g_fd_fb >= 0) {
        fb_set_blank(false);
        close(g_fd_fb);
    }

    if (g_fd_inhibit >= 0) close(g_fd_inhibit);

    log_info("done");
}

static void sig_handler(int s) { (void)s; g_running = 0; }

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    struct sigaction sa = { .sa_handler = sig_handler };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    sigaction(SIGCHLD, &(struct sigaction){ .sa_handler = SIG_DFL }, NULL);

    g_net_menu.parent = &g_root_menu;
    g_pwr_menu.parent = &g_root_menu;

    g_fd_fb      = open(FB_BLANK_PATH,      O_WRONLY | O_CLOEXEC);
    g_fd_inhibit = open(TOUCH_INHIBIT_PATH, O_WRONLY | O_CLOEXEC);
    g_fd_gpio     = open_button_dev(DEV_GPIO);
    g_fd_touchkey = open_button_dev(DEV_TOUCHKEY);

    if (g_fd_fb < 0 || g_fd_inhibit < 0 || g_fd_gpio < 0 || g_fd_touchkey < 0) {
        log_err("open required device");
        cleanup();
        return EXIT_FAILURE;
    }

    if (!terminal_open()) {
        cleanup();
        return EXIT_FAILURE;
    }

    /* Discard events queued before startup */
    {
        struct input_event dummy;
        while (read(g_fd_gpio,     &dummy, sizeof(dummy)) > 0) {}
        while (read(g_fd_touchkey, &dummy, sizeof(dummy)) > 0) {}
    }

    g_menu = &g_root_menu;

    struct pollfd pfds[2] = {
        { .fd = g_fd_gpio,     .events = POLLIN },
        { .fd = g_fd_touchkey, .events = POLLIN },
    };

    log_info("started");

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
