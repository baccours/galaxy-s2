/**
 * menu.c — Hardware button menu for Samsung Galaxy S2 GT-I9100 (pmOS, no GUI)
 *
 * Architecture:
 *   - Finite State Machine: all transitions in one declarative table
 *   - Data-driven menu: menus are const structs; no switch/case in logic
 *   - Parent-pointer back navigation: O(1), no heap, fits a static tree
 *   - Single render() entry point dispatched on app_state
 *   - No shell (fork + execvp only)
 *
 * Device nodes:
 *   event0 : gpio-keys       (Power, Vol+, Vol-)
 *   event1 : tm2-touchkey    (Back, Menu)
 *   event2 : Atmel maXTouch  (Touchscreen — consumed/ignored)
 *
 * Build:  gcc -O2 -Wall -Wextra -o menu menu.c
 * Run:    sudo ./menu
 */

#include <linux/input.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ── Device paths ──────────────────────────────────────────────────────────── */
#define BTN_DEV         "/dev/input/event0"
#define TCH_KEY         "/dev/input/event1"
#define TCH_SCN         "/dev/input/event2"
#define FB_BLANK        "/sys/class/graphics/fb0/blank"
#define BRIGHTNESS_FILE "/sys/class/backlight/spi3.0/brightness"
#define BATTERY_FILE    "/sys/class/power_supply/battery/capacity"

/* ── Key codes ─────────────────────────────────────────────────────────────── */
#define KEY_VOLUMEUP_CODE   115
#define KEY_VOLUMEDOWN_CODE 114
#define KEY_POWER_CODE      116
#define KEY_HOME_CODE       352
#define KEY_MENU_CODE       139
#define KEY_BACK_CODE       158

/* ── Brightness ────────────────────────────────────────────────────────────── */
#define BRIGHTNESS_MIN  0
#define BRIGHTNESS_MAX  24
#define BRIGHTNESS_DEF  8

/* ── Power-button hold thresholds (seconds) ────────────────────────────────── */
#define HOLD_REBOOT_S   2
#define HOLD_POWEROFF_S 5

/* ═══════════════════════════════════════════════════════════════════════════
 * FSM — States & Events
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef enum {
    STATE_IDLE,         /* Screen on, no menu                    */
    STATE_MENU,         /* Menu visible and navigable            */
    STATE_BRIGHTNESS,   /* Brightness adjustment overlay         */
    STATE_BATTERY_INFO, /* Battery reading shown, waiting for key*/
    STATE_COUNT         /* Sentinel — used as wildcard           */
} AppState;

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

/* ═══════════════════════════════════════════════════════════════════════════
 * Data-Driven Menu
 *
 * Each MenuItem is either:
 *   - a leaf   (action != NULL) — calls action() on select
 *   - a node   (submenu != NULL) — navigates into submenu
 *
 * Back navigation uses parent pointers: O(1), no heap, suits a static tree.
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct Menu Menu;
typedef void (*ActionFn)(void);

typedef struct {
    const char *label;
    ActionFn    action;
    const Menu *submenu;
} MenuItem;

struct Menu {
    const char     *title;
    const MenuItem *items;
    uint8_t         count;
    const Menu     *parent;   /* NULL for root */
};

/* ── Forward declarations for actions ─────────────────────────────────────── */
static void action_battery(void);
static void action_brightness_enter(void);
static void action_wifi_toggle(void);
static void action_ssh_restart(void);
static void action_fbkeyboard_toggle(void);
static void action_exit_menu(void);

/* ── Networking submenu ────────────────────────────────────────────────────── */
static const MenuItem NETWORK_ITEMS[] = {
    { "Toggle WiFi", action_wifi_toggle, NULL },
    { "Restart SSH",  action_ssh_restart, NULL },
};
/* Parent pointer set at runtime in main() — can't take address of ROOT_MENU here */
static Menu NETWORK_MENU = {
    .title  = "Networking",
    .items  = NETWORK_ITEMS,
    .count  = (uint8_t)(sizeof(NETWORK_ITEMS) / sizeof(NETWORK_ITEMS[0])),
    .parent = NULL   /* linked in main() */
};

/* ── Root menu — edit only this table to add/remove/reorder entries ────────── */
static const MenuItem ROOT_ITEMS[] = {
    { "Check Battery",     action_battery,           NULL           },
    { "Adjust Brightness", action_brightness_enter,  NULL           },
    { "Networking",        NULL,                     &NETWORK_MENU  },
    { "Toggle Keyboard",   action_fbkeyboard_toggle, NULL           },
    { "Exit Menu",         action_exit_menu,         NULL           },
};
static const Menu ROOT_MENU = {
    .title  = "GT-I9100 pmOS",
    .items  = ROOT_ITEMS,
    .count  = (uint8_t)(sizeof(ROOT_ITEMS) / sizeof(ROOT_ITEMS[0])),
    .parent = NULL
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Global runtime state
 * ═══════════════════════════════════════════════════════════════════════════ */
static volatile sig_atomic_t keep_running = 1;

static AppState        app_state          = STATE_IDLE;
static bool            screen_blanked     = false;
static uint8_t         current_brightness = BRIGHTNESS_DEF;
static const Menu     *current_menu       = NULL;  /* active menu node     */
static uint8_t         selection          = 0;     /* highlighted item idx  */
static struct timespec power_press_time;

static void handle_sig(int sig) { (void)sig; keep_running = 0; }

/* ═══════════════════════════════════════════════════════════════════════════
 * Low-level helpers
 * ═══════════════════════════════════════════════════════════════════════════ */
static int write_sysfs(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t len = (ssize_t)strlen(value);
    ssize_t ret = write(fd, value, (size_t)len);
    close(fd);
    return (ret == len) ? 0 : -1;
}

static int read_sysfs(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
        buf[--n] = '\0';
    return (int)n;
}

static int run_cmd(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) { execvp(argv[0], argv); _exit(127); }
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void set_brightness(int level)
{
    if (level < BRIGHTNESS_MIN) level = BRIGHTNESS_MIN;
    if (level > BRIGHTNESS_MAX) level = BRIGHTNESS_MAX;
    current_brightness = (uint8_t)level;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", level);
    if (write_sysfs(BRIGHTNESS_FILE, buf) < 0) perror("set_brightness");
}

static void set_blank(int fb_fd, bool blank)
{
    const char val = blank ? '1' : '0';
    if (write(fb_fd, &val, 1) < 0) perror("set_blank");
    else screen_blanked = blank;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Rendering — single entry point, dispatched on app_state
 * ═══════════════════════════════════════════════════════════════════════════ */
static void render_menu_view(void)
{
    /* Breadcrumb: walk from root to current via parent chain */
    /* Collect path first (max depth is small) */
    const Menu *path[16];
    int depth = 0;
    for (const Menu *m = current_menu; m && depth < 16; m = m->parent)
        path[depth++] = m;
    for (int i = depth - 1; i >= 0; i--) {
        if (i < depth - 1) fputs(" > ", stdout);
        fputs(path[i]->title, stdout);
    }
    fputs("\r\n\r\n", stdout);

    for (uint8_t i = 0; i < current_menu->count; i++) {
        bool selected = (i == selection);
        bool has_sub  = (current_menu->items[i].submenu != NULL);
        if (selected)
            printf("\033[7m > %-22s%s\033[0m\r\n",
                   current_menu->items[i].label, has_sub ? " >" : "  ");
        else
            printf("   %-22s%s\r\n",
                   current_menu->items[i].label, has_sub ? " >" : "  ");
    }
    fputs("\r\n[Vol+/-]: Nav  [Power]: Select  [Back]: Back\r\n", stdout);
}

static void render_brightness_view(void)
{
    printf("--- Brightness ---\r\n\r\n"
           " Level: %u / %d\r\n [",
           current_brightness, BRIGHTNESS_MAX);
    for (int i = 0; i < BRIGHTNESS_MAX; i++)
        putchar(i < current_brightness ? '#' : '-');
    fputs("]\r\n\r\n[Vol+/-]: Change  [Power/Back]: Done\r\n", stdout);
}

static void render_battery_view(void)
{
    char buf[16] = "??";
    read_sysfs(BATTERY_FILE, buf, sizeof(buf));
    printf(" Battery: %s%%\r\n\r\n [Any key to return]\r\n", buf);
}

/* Unified render — always clears screen first, then delegates */
static void render(void)
{
    fputs("\033[H\033[J", stdout);
    switch (app_state) {
        case STATE_MENU:         render_menu_view();       break;
        case STATE_BRIGHTNESS:   render_brightness_view(); break;
        case STATE_BATTERY_INFO: render_battery_view();    break;
        case STATE_IDLE:         /* nothing to draw */     break;
        default:                                           break;
    }
    fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Menu actions
 * ═══════════════════════════════════════════════════════════════════════════ */
static void action_battery(void)
{
    app_state = STATE_BATTERY_INFO;
    /* render() called by FSM after action returns */
}

static void action_brightness_enter(void)
{
    app_state = STATE_BRIGHTNESS;
}

static void action_wifi_toggle(void)
{
    FILE *fp = popen("nmcli -t -f WIFI radio", "r");
    if (!fp) { perror("popen nmcli"); return; }
    char state[16] = {0};
    if (fgets(state, sizeof(state), fp))
        state[strcspn(state, "\r\n")] = '\0';
    pclose(fp);

    if (strncmp(state, "enabled", 7) == 0) {
        char *const a[] = { "nmcli", "radio", "wifi", "off", NULL };
        run_cmd((char *const *)a);
    } else {
        char *const a[] = { "nmcli", "radio", "wifi", "on", NULL };
        run_cmd((char *const *)a);
    }
}

static void action_ssh_restart(void)
{
    char *const a[] = { "rc-service", "sshd", "restart", NULL };
    run_cmd((char *const *)a);
}

static void action_fbkeyboard_toggle(void)
{
    char *const status[] = { "rc-service", "fbkeyboard", "status", NULL };
    char *const start[]  = { "rc-service", "fbkeyboard", "start",  NULL };
    char *const stop[]   = { "rc-service", "fbkeyboard", "stop",   NULL };
    run_cmd(run_cmd((char *const *)status) == 0
            ? (char *const *)stop
            : (char *const *)start);
}

static void action_exit_menu(void)
{
    current_menu = &ROOT_MENU;
    selection    = 0;
    app_state    = STATE_IDLE;
}

/* ── Execute the highlighted item ─────────────────────────────────────────── */
static void menu_select(void)
{
    const MenuItem *item = &current_menu->items[selection];
    if (item->submenu) {
        current_menu = item->submenu;
        selection    = 0;
        /* stay in STATE_MENU */
    } else if (item->action) {
        item->action();
        /* action may have changed app_state (e.g. STATE_BRIGHTNESS) */
    }
}

/* ── Navigate back via parent pointer ─────────────────────────────────────── */
static void menu_back(void)
{
    if (current_menu->parent) {
        current_menu = current_menu->parent;
        selection    = 0;
    } else {
        /* Already at root — back exits the menu */
        action_exit_menu();
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * FSM — Transition table
 *
 * { current_state, event, handler }
 * STATE_COUNT = wildcard, matches any state.
 * First match wins — specific states must appear before wildcards.
 *
 * Every handler receives fb_fd for screen-blank control.
 * Handlers that don't need it cast it to void.
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef void (*TransitionFn)(int fb_fd);

typedef struct {
    AppState     state;
    FsmEvent     event;
    TransitionFn handler;
} Transition;

/* ── Transition handlers ───────────────────────────────────────────────────── */
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
static void th_idle_power(int fb_fd) { set_blank(fb_fd, !screen_blanked); }
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
static void th_home(int _) { (void)_; action_fbkeyboard_toggle(); }

/* ── Table ─────────────────────────────────────────────────────────────────── */
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

static void fsm_dispatch(FsmEvent evt, int fb_fd)
{
    for (size_t i = 0; i < TRANSITION_COUNT; i++) {
        const Transition *t = &TRANSITIONS[i];
        if ((t->state == STATE_COUNT || t->state == app_state) && t->event == evt) {
            t->handler(fb_fd);
            return;
        }
    }
    /* Unhandled event in this state — silently ignored by design */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Input → FSM event translation
 * ═══════════════════════════════════════════════════════════════════════════ */
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

    if (ev->value != 1) return; /* Only key-down for the rest */
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

/* ═══════════════════════════════════════════════════════════════════════════
 * Cleanup
 * ═══════════════════════════════════════════════════════════════════════════ */
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

/* ═══════════════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    struct pollfd fds[3];
    struct input_event ev;
    int fb_fd = -1;
    int ret   = EXIT_SUCCESS;

    signal(SIGINT,  handle_sig);
    signal(SIGTERM, handle_sig);
    signal(SIGCHLD, SIG_DFL);    /* Reap fork()ed children automatically */

    /* Link parent pointers for submenus (can't do this in static initialiser) */
    NETWORK_MENU.parent = &ROOT_MENU;

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

    current_menu = &ROOT_MENU;
    set_brightness(BRIGHTNESS_DEF);

    while (keep_running) {
        int n = poll(fds, 3, -1);
        if (n < 0) {
            if (errno == EINTR) continue;   /* Signal (e.g. SIGCHLD) — retry */
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
                ; /* Consume touchscreen events silently */
    }

done:
    cleanup(fds, 3, fb_fd);
    return ret;
}

