/**
 * main.c — GT-I9100 pmOS hardware button menu
 *
 * Responsibilities of this file ONLY:
 *   - Open/grab/release device nodes
 *   - Run the poll() event loop
 *   - Translate raw input_event → FsmEvent and call fsm_dispatch()
 *   - Signal handling and clean shutdown
 *
 * Everything else lives in its own module:
 *   config.h   — compile-time constants
 *   sysfs.[ch] — sysfs I/O, run_cmd, set_brightness, set_blank
 *   menu.[ch]  — menu tree, navigation state, nav API
 *   actions.[ch]— menu action callbacks
 *   render.[ch] — terminal rendering
 *   fsm.[ch]   — FSM states/events, transition table, dispatcher
 */

#include "config.h"
#include "fsm.h"
#include "menu.h"
#include "sysfs.h"

#include <linux/input.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* ── Signal handler ────────────────────────────────────────────────────────── */
static volatile sig_atomic_t keep_running = 1;
static void handle_sig(int sig) { (void)sig; keep_running = 0; }

/* ── Power-button timing ───────────────────────────────────────────────────── */
static struct timespec power_press_time;

/* ── Input translation ─────────────────────────────────────────────────────── */
static void process_btn_event(const struct input_event *ev, int fb_fd)
{
    if (ev->type != EV_KEY) return;

    if (ev->code == KEY_POWER_CODE) {
        if (ev->value == 1) {
            clock_gettime(CLOCK_MONOTONIC, &power_press_time);
        } else if (ev->value == 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long diff = now.tv_sec - power_press_time.tv_sec;
            FsmEvent evt = (diff >= HOLD_POWEROFF_S) ? EVT_POWER_LONG_OFF
                         : (diff >= HOLD_REBOOT_S)   ? EVT_POWER_LONG_REBOOT
                                                      : EVT_POWER_SHORT;
            fsm_dispatch(evt, fb_fd);
        }
        return;
    }

    if (ev->value != 1) return; /* key-down only for the rest */
    switch (ev->code) {
        case KEY_VOLUMEUP_CODE:   fsm_dispatch(EVT_VOL_UP,   fb_fd); break;
        case KEY_VOLUMEDOWN_CODE: fsm_dispatch(EVT_VOL_DOWN, fb_fd); break;
        case KEY_HOME_CODE:       fsm_dispatch(EVT_HOME_KEY, fb_fd); break;
        default: break;
    }
}

static void process_tch_key_event(const struct input_event *ev, int fb_fd)
{
    if (ev->type != EV_KEY || ev->value != 1) return;
    switch (ev->code) {
        case KEY_MENU_CODE: fsm_dispatch(EVT_MENU_KEY, fb_fd); break;
        case KEY_BACK_CODE: fsm_dispatch(EVT_BACK_KEY, fb_fd); break;
        default: break;
    }
}

/* ── Cleanup ───────────────────────────────────────────────────────────────── */
static void cleanup(struct pollfd *fds, int count, int fb_fd)
{
    fputs("\r\nReleasing devices and exiting...\r\n", stdout);
    for (int i = 0; i < count; i++) {
        if (fds[i].fd >= 0) {
            ioctl(fds[i].fd, EVIOCGRAB, 0);
            close(fds[i].fd);
        }
    }
    if (fb_fd >= 0) close(fb_fd);
}

/* ── Main ──────────────────────────────────────────────────────────────────── */
int main(void)
{
    struct pollfd fds[3];
    struct input_event ev;
    int fb_fd = -1;
    int ret   = EXIT_SUCCESS;

    signal(SIGINT,  handle_sig);
    signal(SIGTERM, handle_sig);
    signal(SIGCHLD, SIG_DFL);  /* reap fork()ed children automatically */

    menu_init();               /* link parent pointers, set current_menu */
    set_brightness(BRIGHTNESS_DEF);

    fb_fd     = open(FB_BLANK, O_WRONLY);
    fds[0].fd = open(BTN_DEV,  O_RDONLY | O_NONBLOCK);
    fds[1].fd = open(TCH_KEY,  O_RDONLY | O_NONBLOCK);
    fds[2].fd = open(TCH_SCN,  O_RDONLY | O_NONBLOCK);

    if (fds[0].fd < 0 || fds[1].fd < 0 || fds[2].fd < 0 || fb_fd < 0) {
        perror("Failed to open device nodes");
        ret = EXIT_FAILURE;
        goto done;
    }

    for (int i = 0; i < 3; i++) {
        if (ioctl(fds[i].fd, EVIOCGRAB, 1) < 0) perror("EVIOCGRAB");
        fds[i].events = POLLIN;
    }

    while (keep_running) {
        int n = poll(fds, 3, -1);
        if (n < 0) {
            if (errno == EINTR) continue;  /* SIGCHLD or other signal — retry */
            perror("poll");
            ret = EXIT_FAILURE;
            break;
        }

        if (fds[0].revents & POLLIN)
            while (read(fds[0].fd, &ev, sizeof(ev)) > 0)
                process_btn_event(&ev, fb_fd);

        if (fds[1].revents & POLLIN)
            while (read(fds[1].fd, &ev, sizeof(ev)) > 0)
                process_tch_key_event(&ev, fb_fd);

        if (fds[2].revents & POLLIN)
            while (read(fds[2].fd, &ev, sizeof(ev)) > 0)
                ; /* consume touchscreen events silently */
    }

done:
    cleanup(fds, 3, fb_fd);
    return ret;
}
