/*
 * pmenu.c — postmarketOS hardware-button menu for Samsung Galaxy S2 GT-I9100
 *
 * Architecture
 *   ┌─────────────────────────────────────────────────────────────────┐
 *   │  Declarative FSM transition table                               │
 *   │    { state, event, handler }  — STATE_ANY = wildcard            │
 *   │    First match wins; global events listed after specific ones   │
 *   ├─────────────────────────────────────────────────────────────────┤
 *   │  Data-driven menu tree                                          │
 *   │    Menu structs carry parent pointers — O(1) back, no heap      │
 *   │    MenuItem = { label, action | submenu }                       │
 *   ├─────────────────────────────────────────────────────────────────┤
 *   │  Single render() entry point                                    │
 *   │    Dispatches on app_state — every handler ends with render()   │
 *   ├─────────────────────────────────────────────────────────────────┤
 *   │  fb_blank fd held open; fork+execvp (no shell)                  │
 *   │  poll(2) on 2 fds — appropriate for this device count           │
 *   └─────────────────────────────────────────────────────────────────┘
 *
 * Input devices
 *   event0  gpio-keys     VOL+(115) VOL-(114) POWER(116) HOME(352)
 *   event1  tm2-touchkey  MENU(139) BACK(158)
 *   event2  Touchscreen   grabbed/released as needed
 *   event3  fbkeyboard    grabbed/released as needed
 *
 * Build:  gcc -O2 -Wall -Wextra -std=c11 -o pmenu pmenu.c
 * Run:    sudo ./pmenu
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define DEV_GPIO        "/dev/input/event0"
#define DEV_TOUCHKEY    "/dev/input/event1"
#define DEV_TOUCH       "/dev/input/event2"
#define DEV_FBKBD       "/dev/input/event3"
#define FB_BLANK_PATH   "/sys/class/graphics/fb0/blank"

#define KEY_VOLUMEUP_CODE   115
#define KEY_VOLUMEDOWN_CODE 114
#define KEY_POWER_CODE      116
#define KEY_HOME_CODE       352
#define KEY_MENU_CODE       139
#define KEY_BACK_CODE       158

/* ANSI helpers */
#define T_CLEAR  "\033[H\033[J"
#define T_RESET  "\033[0m"
#define T_BOLD   "\033[1m"
#define T_DIM    "\033[2m"
#define T_REV    "\033[7m"
#define T_CYAN   "\033[36m"
#define T_YELLOW "\033[33m"
#define T_HIDE   "\033[?25l"
#define T_SHOW   "\033[?25h"

#define BOX_W 30   /* printable width of the menu box interior */

/* ═══════════════════════════════════════════════════════════════════════════
 * FSM — States & Events
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    STATE_IDLE,     /* Screen on, terminal visible, no menu    */
    STATE_MENU,     /* Menu visible and navigable              */
    STATE_ANY,      /* Wildcard — matches any state in table   */
} AppState;

typedef enum {
    EVT_VOL_UP,
    EVT_VOL_DOWN,
    EVT_POWER,
    EVT_MENU_KEY,
    EVT_BACK_KEY,
    EVT_HOME_KEY,
} FsmEvent;

/* ═══════════════════════════════════════════════════════════════════════════
 * Data-driven menu tree
 *
 *  Menu        — a titled list of MenuItems + parent pointer
 *  MenuItem    — either a leaf (action != NULL) or a node (submenu != NULL)
 *
 *  Parent pointers are set once at runtime in main() because C static
 *  initialisers cannot take the address of another object in the same TU
 *  before it is fully defined.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct Menu Menu;
typedef void (*ActionFn)(void);

typedef struct {
    const char *label;
    ActionFn    action;     /* NULL for submenu items  */
    Menu       *submenu;    /* NULL for leaf items     */
} MenuItem;

struct Menu {
    const char  *title;
    MenuItem    *items;
    uint8_t      count;
    const Menu  *parent;   /* NULL at root; set at runtime for submenus */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Global runtime state
 * ═══════════════════════════════════════════════════════════════════════════ */

static volatile sig_atomic_t g_running = 1;

static AppState    g_state        = STATE_IDLE;
static bool        g_screen_blank = false;
static bool        g_fbkbd_on     = false;   /* fbkeyboard service running? */

static const Menu *g_menu         = NULL;   /* active menu node            */
static uint8_t     g_sel          = 0;      /* highlighted item index      */

/* File descriptors — held open for lifetime of process */
static int g_fd_gpio     = -1;
static int g_fd_touchkey = -1;
static int g_fd_touch    = -1;
static int g_fd_fbkbd    = -1;
static int g_fd_fb       = -1;   /* FB_BLANK_PATH, O_WRONLY */

/* Terminal */
static struct termios g_orig_termios;
static bool           g_termios_saved = false;

/* ═══════════════════════════════════════════════════════════════════════════
 * Logging
 * ═══════════════════════════════════════════════════════════════════════════ */

static void log_info(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fputs("[pmenu] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void log_err(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[pmenu] ERROR: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, ": %s\n", strerror(errno));
    va_end(ap);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Terminal helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void terminal_raw(void)
{
    struct termios t;
    if (tcgetattr(STDOUT_FILENO, &t) != 0) return;
    g_orig_termios  = t;
    g_termios_saved = true;
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
    t.c_cc[VMIN]  = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDOUT_FILENO, TCSAFLUSH, &t);
}

static void terminal_restore(void)
{
    if (g_termios_saved)
        tcsetattr(STDOUT_FILENO, TCSAFLUSH, &g_orig_termios);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Low-level system helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Write blank/unblank to the already-open fb sysfs fd.
 * lseek back to 0 so the next write lands at offset 0 again.
 */
static void fb_set_blank(bool blank)
{
    const char c = blank ? '1' : '0';
    if (write(g_fd_fb, &c, 1) < 0) log_err("fb_blank write");
    else g_screen_blank = blank;
    lseek(g_fd_fb, 0, SEEK_SET);
    log_info("screen %s", blank ? "blanked" : "unblanked");
}

/* fork + execvp — no shell, no injection surface */
static int run_cmd(char *const argv[])
{
    log_info("exec: %s", argv[0]);
    pid_t pid = fork();
    if (pid < 0) { log_err("fork"); return -1; }
    if (pid == 0) { execvp(argv[0], argv); _exit(127); }
    int st;
    if (waitpid(pid, &st, 0) < 0) { log_err("waitpid"); return -1; }
    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (rc != 0) log_info("%s returned %d", argv[0], rc);
    return rc;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Device grab helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void grab(int fd, bool on)
{
    if (fd < 0) return;
    if (ioctl(fd, EVIOCGRAB, on ? (void *)1 : (void *)0) < 0)
        log_err("EVIOCGRAB");
}

/*
 * Touchscreen grab policy:
 *   grabbed (input swallowed) when menu is open OR screen is blank.
 *
 * fbkeyboard device grab policy:
 *   grabbed when menu is open (prevents stray chars in the terminal).
 */
static void update_grabs(void)
{
    bool menu_on = (g_state == STATE_MENU);
    grab(g_fd_touch, menu_on || g_screen_blank);
    grab(g_fd_fbkbd, menu_on);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * fbkeyboard OpenRC service
 * ═══════════════════════════════════════════════════════════════════════════ */

static void fbkbd_set(bool start)
{
    char *const a_start[] = { "rc-service", "fbkeyboard", "start", NULL };
    char *const a_stop[]  = { "rc-service", "fbkeyboard", "stop",  NULL };
    if (run_cmd(start ? a_start : a_stop) == 0)
        g_fbkbd_on = start;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Rendering — single entry point
 * ═══════════════════════════════════════════════════════════════════════════ */

static void render_menu(void)
{
    /* Breadcrumb: collect path from root to current via parent chain */
    const Menu *path[16];
    int depth = 0;
    for (const Menu *m = g_menu; m && depth < 16; m = m->parent)
        path[depth++] = m;

    fputs(T_BOLD T_CYAN, stdout);

    /* Top border */
    fputs("+", stdout);
    for (int i = 0; i < BOX_W; i++) fputc('-', stdout);
    fputs("+\r\n", stdout);

    /* Breadcrumb row */
    fputs("| " T_YELLOW, stdout);
    int used = 0;
    for (int i = depth - 1; i >= 0; i--) {
        int n = fprintf(stdout, "%s%s", path[i]->title, i > 0 ? " > " : "");
        if (n > 0) used += n;
    }
    for (int i = used; i < BOX_W - 2; i++) fputc(' ', stdout);
    fputs(T_CYAN " |\r\n", stdout);

    /* Separator */
    fputs("+", stdout);
    for (int i = 0; i < BOX_W; i++) fputc('-', stdout);
    fputs("+\r\n", stdout);

    /* Items */
    for (uint8_t i = 0; i < g_menu->count; i++) {
        bool sel     = (i == g_sel);
        bool has_sub = (g_menu->items[i].submenu != NULL);
        const char *suffix = has_sub ? " >" : "  ";
        if (sel)
            printf("| " T_REV T_BOLD "%-*s%s" T_RESET T_CYAN " |\r\n",
                   BOX_W - 5, g_menu->items[i].label, suffix);
        else
            printf("| " T_RESET "%-*s%s" T_CYAN " |\r\n",
                   BOX_W - 5, g_menu->items[i].label, suffix);
    }

    /* Bottom border */
    fputs(T_CYAN "+", stdout);
    for (int i = 0; i < BOX_W; i++) fputc('-', stdout);
    fputs("+\r\n", stdout);

    fputs(T_DIM "VOL+/-: navigate   PWR: select   BACK: back\r\n"
          T_RESET, stdout);
}

/* Unified render — always clears, then delegates on g_state */
static void render(void)
{
    fputs(T_CLEAR, stdout);
    if (g_state == STATE_MENU)
        render_menu();
    fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Menu actions (leaf callbacks)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void action_wifi_toggle(void)
{
    char *const chk[] = { "sh", "-c",
        "rfkill list wifi | grep -q 'Soft blocked: no'", NULL };
    bool enabled = (run_cmd(chk) == 0);
    char *const off[] = { "rfkill", "block",   "wifi", NULL };
    char *const on[]  = { "rfkill", "unblock", "wifi", NULL };
    run_cmd(enabled ? off : on);
}

static void action_bt_toggle(void)
{
    char *const chk[] = { "sh", "-c",
        "rfkill list bluetooth | grep -q 'Soft blocked: no'", NULL };
    bool enabled = (run_cmd(chk) == 0);
    char *const off[] = { "rfkill", "block",   "bluetooth", NULL };
    char *const on[]  = { "rfkill", "unblock", "bluetooth", NULL };
    run_cmd(enabled ? off : on);
}

static void action_reboot(void)
{
    char *const a[] = { "reboot", NULL };
    run_cmd(a);
}

static void action_poweroff(void)
{
    char *const a[] = { "poweroff", NULL };
    run_cmd(a);
}

/* Forward-declared; body after th_close_menu is defined */
static void action_exit_menu(void);

/* ═══════════════════════════════════════════════════════════════════════════
 * Menu tree — edit only these tables to extend the menu
 * ═══════════════════════════════════════════════════════════════════════════ */

static MenuItem g_net_items[] = {
    { "Toggle WiFi",      action_wifi_toggle, NULL },
    { "Toggle Bluetooth", action_bt_toggle,   NULL },
};
static Menu g_net_menu = {
    "Networking", g_net_items, 2, NULL   /* parent linked in main() */
};

static MenuItem g_pwr_items[] = {
    { "Reboot",    action_reboot,   NULL },
    { "Power Off", action_poweroff, NULL },
};
static Menu g_pwr_menu = {
    "Power", g_pwr_items, 2, NULL        /* parent linked in main() */
};

static MenuItem g_root_items[] = {
    { "Networking", NULL, &g_net_menu },
    { "Power",      NULL, &g_pwr_menu },
    { "Exit Menu",  action_exit_menu, NULL },
};
static const Menu g_root_menu = {
    "pmOS  GT-I9100", g_root_items, 3, NULL
};

/* ═══════════════════════════════════════════════════════════════════════════
 * FSM — Transition handlers
 *
 * Each handler mutates global state then calls render().
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
    } else if (item->action) {
        item->action();
        /* action may have changed g_state (e.g. action_exit_menu) */
    }
    render();
}

static void th_close_menu(void)
{
    g_menu  = &g_root_menu;
    g_sel   = 0;
    g_state = STATE_IDLE;
    update_grabs();
    fputs(T_SHOW, stdout);
    render();
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
    if (g_fbkbd_on) fbkbd_set(false);    /* hide keyboard while in menu */
    g_menu  = &g_root_menu;
    g_sel   = 0;
    g_state = STATE_MENU;
    update_grabs();
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

/* action_exit_menu is a leaf action that needs th_close_menu */
static void action_exit_menu(void) { th_close_menu(); }

/* ═══════════════════════════════════════════════════════════════════════════
 * FSM — Declarative transition table
 *
 *   { state, event, handler }
 *   STATE_ANY = wildcard, matches any current state.
 *   First match wins — specific states must appear before STATE_ANY rows.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef void (*TransitionFn)(void);

typedef struct {
    AppState     state;
    FsmEvent     event;
    TransitionFn handler;
} Transition;

static const Transition TRANSITIONS[] = {
    /* ── Menu navigation (STATE_MENU before STATE_ANY) ───────────────── */
    { STATE_MENU, EVT_VOL_UP,    th_menu_up     },
    { STATE_MENU, EVT_VOL_DOWN,  th_menu_down   },
    { STATE_MENU, EVT_POWER,     th_menu_select },
    { STATE_MENU, EVT_BACK_KEY,  th_menu_back   },
    { STATE_MENU, EVT_MENU_KEY,  th_close_menu  },

    /* ── Idle-specific ────────────────────────────────────────────────── */
    { STATE_IDLE, EVT_POWER,     th_idle_power  },

    /* ── Global: STATE_ANY catches remaining states / events ──────────── */
    /* th_idle_power above fires for STATE_IDLE (screen on → toggle blank).
     * For any other state where POWER wasn't matched (currently: screen
     * already blank in STATE_IDLE won't reach here since th_idle_power
     * handles both), th_idle_power's own guard handles the no-op.        */
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

/* Drain all pending events from one fd */
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
 * Initialisation & cleanup
 * ═══════════════════════════════════════════════════════════════════════════ */

static int open_dev(const char *path, bool grab_now)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { log_err("open %s", path); return -1; }
    if (grab_now && ioctl(fd, EVIOCGRAB, (void *)1) < 0)
        log_err("EVIOCGRAB %s", path);
    return fd;
}

static void sig_handler(int s) { (void)s; g_running = 0; }

static void cleanup(void)
{
    /* 1. Restore terminal first — most critical for the user's session.
     *    g_termios_saved guards against calling this before terminal_raw(). */
    terminal_restore();
    fputs(T_CLEAR T_RESET T_SHOW, stdout);
    fflush(stdout);

    /* 2. Release input grabs — device usable again immediately after. */
    grab(g_fd_gpio,     false);
    grab(g_fd_touchkey, false);
    grab(g_fd_touch,    false);
    grab(g_fd_fbkbd,    false);

    /* 3. Close file descriptors. */
    if (g_fd_gpio     >= 0) close(g_fd_gpio);
    if (g_fd_touchkey >= 0) close(g_fd_touchkey);
    if (g_fd_touch    >= 0) close(g_fd_touch);
    if (g_fd_fbkbd    >= 0) close(g_fd_fbkbd);

    if (g_fd_fb >= 0) {
        fb_set_blank(false);
        close(g_fd_fb);
    }

    log_info("done");
}

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
    /* Reap fork()ed children automatically */
    sigaction(SIGCHLD, &(struct sigaction){ .sa_handler = SIG_DFL }, NULL);

    /* Link parent pointers — cannot be done in static initialisers */
    g_net_menu.parent = &g_root_menu;
    g_pwr_menu.parent = &g_root_menu;

    /* Open fb_blank once; reused for every screen-blank write */
    g_fd_fb = open(FB_BLANK_PATH, O_WRONLY | O_CLOEXEC);
    if (g_fd_fb < 0) { log_err("open " FB_BLANK_PATH); return EXIT_FAILURE; }

    /* Button devices: grabbed permanently — we are the sole consumer.
     * Touch / fbkbd: opened ungrabbed; grab managed by update_grabs(). */
    g_fd_gpio     = open_dev(DEV_GPIO,     true);
    g_fd_touchkey = open_dev(DEV_TOUCHKEY, true);
    g_fd_touch    = open_dev(DEV_TOUCH,    false);
    g_fd_fbkbd    = open_dev(DEV_FBKBD,    false);

    if (g_fd_gpio < 0 || g_fd_touchkey < 0) {
        log_info("Cannot open required button devices — aborting.");
        cleanup();
        return EXIT_FAILURE;
    }

    /* poll(2) on 2 fds — perfectly adequate for this device count */
    struct pollfd pfds[2] = {
        { .fd = g_fd_gpio,     .events = POLLIN },
        { .fd = g_fd_touchkey, .events = POLLIN },
    };

    g_menu = &g_root_menu;
    /* terminal_raw() only after all devices confirmed open — avoids dirtying
     * the terminal on a failed startup (cleanup() guards with g_termios_saved
     * but it is cleaner never to touch it in the first place). */
    terminal_raw();
    fputs(T_CLEAR T_SHOW, stdout);
    fflush(stdout);
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