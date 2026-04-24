/*
 * pmenu.c — postmarketOS hardware-button menu for Samsung Galaxy S2 GT-I9100
 *
 * State machine + data-driven menu system.
 * Grabs all relevant input devices exclusively so no other process
 * competes for button events.
 *
 * Devices
 *   /dev/input/event0  gpio-keys      (VOL+/VOL-/POWER/HOME)
 *   /dev/input/event1  tm2-touchkey   (MENU / BACK)
 *   /dev/input/event2  Touchscreen    (grabbed/released as needed)
 *   /dev/input/event3  fbkeyboard     (grabbed/released as needed)
 *
 * Key codes
 *   KEY_VOLUMEUP   115
 *   KEY_VOLUMEDOWN 114
 *   KEY_POWER      116
 *   KEY_HOME       352   (gpio-keys names it KEY_CAMERA on some kernels,
 *                         but the scancode maps to 352 on this device)
 *   KEY_MENU       139
 *   KEY_BACK       158
 *
 * Build
 *   gcc -O2 -Wall -o pmenu pmenu.c
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define DEV_GPIO     "/dev/input/event0"
#define DEV_TOUCHKEY "/dev/input/event1"
#define DEV_TOUCH    "/dev/input/event2"
#define DEV_FBKBD    "/dev/input/event3"
#define FB_BLANK     "/sys/class/graphics/fb0/blank"

#define KEY_VOLUMEUP_CODE   115
#define KEY_VOLUMEDOWN_CODE 114
#define KEY_POWER_CODE      116
#define KEY_HOME_CODE       352
#define KEY_MENU_CODE       139
#define KEY_BACK_CODE       158

#define MAX_ITEMS   16
#define MAX_DEPTH    8   /* maximum menu nesting depth */

/* ANSI escape helpers */
#define ANSI_CLEAR      "\033[2J\033[H"
#define ANSI_RESET      "\033[0m"
#define ANSI_BOLD       "\033[1m"
#define ANSI_REV        "\033[7m"       /* reverse video = highlight */
#define ANSI_DIM        "\033[2m"
#define ANSI_CYAN       "\033[36m"
#define ANSI_YELLOW     "\033[33m"
#define ANSI_RED        "\033[31m"

/* -------------------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------------- */

typedef struct MenuItem MenuItem;
typedef void (*ActionFn)(void);

/* -------------------------------------------------------------------------
 * Menu data structures
 * ---------------------------------------------------------------------- */

struct MenuItem {
    const char *label;
    ActionFn    action;      /* NULL when item leads to a submenu */
    MenuItem   *submenu;     /* NULL when item is a leaf action  */
    int         submenu_len;
};

/* -------------------------------------------------------------------------
 * Global state
 * ---------------------------------------------------------------------- */

/* Raw terminal state backup */
static struct termios g_orig_termios;
static bool g_termios_saved = false;

/* File descriptors */
static int g_fd_gpio     = -1;
static int g_fd_touchkey = -1;
static int g_fd_touch    = -1;
static int g_fd_fbkbd    = -1;
static int g_epfd        = -1;

/* Screen / display state */
static bool g_screen_blank   = false;
static bool g_menu_visible   = false;
static bool g_fbkbd_visible  = false;  /* fbkeyboard shown outside menu */

/* Navigation stack */
static MenuItem *g_nav_stack[MAX_DEPTH];
static int       g_nav_len_stack[MAX_DEPTH]; /* item count at each level */
static int       g_nav_sel_stack[MAX_DEPTH]; /* selected index at each level */
static int       g_nav_depth = 0;

/* Current menu state */
static MenuItem *g_current_menu = NULL;
static int       g_current_len  = 0;
static int       g_current_sel  = 0;

static volatile sig_atomic_t g_running = 1;

/* -------------------------------------------------------------------------
 * Logging helpers
 * ---------------------------------------------------------------------- */

static void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[pmenu] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void log_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[pmenu] ERROR: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, ": %s\n", strerror(errno));
    va_end(ap);
}

/* -------------------------------------------------------------------------
 * Terminal helpers
 * ---------------------------------------------------------------------- */

static void terminal_raw(void)
{
    struct termios t;
    if (tcgetattr(STDOUT_FILENO, &t) == 0) {
        g_orig_termios  = t;
        g_termios_saved = true;
        t.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
        t.c_cc[VMIN]  = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDOUT_FILENO, TCSAFLUSH, &t);
    }
}

static void terminal_restore(void)
{
    if (g_termios_saved)
        tcsetattr(STDOUT_FILENO, TCSAFLUSH, &g_orig_termios);
}

static void cursor_hide(void)  { fputs("\033[?25l", stdout); fflush(stdout); }
static void cursor_show(void)  { fputs("\033[?25h", stdout); fflush(stdout); }

/* -------------------------------------------------------------------------
 * sysfs / service helpers
 * ---------------------------------------------------------------------- */

static void write_sysfs(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) { log_err("open %s", path); return; }
    if (write(fd, value, strlen(value)) < 0) log_err("write %s", path);
    close(fd);
}

static void run_cmd(const char *cmd)
{
    log_info("exec: %s", cmd);
    int rc = system(cmd);
    if (rc != 0)
        log_info("command returned %d", rc);
}

/* -------------------------------------------------------------------------
 * Device grab helpers
 * ---------------------------------------------------------------------- */

static int grab_device(int fd, bool grab)
{
    return ioctl(fd, EVIOCGRAB, grab ? (void *)1 : (void *)0);
}

/* Touchscreen: grabbed (disabled) when menu is open or screen is blank */
static void touchscreen_update(void)
{
    if (g_fd_touch < 0) return;
    bool should_grab = g_menu_visible || g_screen_blank;
    if (grab_device(g_fd_touch, should_grab) < 0)
        log_err("EVIOCGRAB touchscreen");
}

/* fbkeyboard device: grabbed when menu is visible (keyboard hidden in menu) */
static void fbkbd_grab_update(void)
{
    if (g_fd_fbkbd < 0) return;
    if (grab_device(g_fd_fbkbd, g_menu_visible) < 0)
        log_err("EVIOCGRAB fbkeyboard");
}

/* -------------------------------------------------------------------------
 * fbkeyboard OpenRC service
 * ---------------------------------------------------------------------- */

static void fbkbd_service(bool start)
{
    if (start) {
        run_cmd("rc-service fbkeyboard start");
        g_fbkbd_visible = true;
    } else {
        run_cmd("rc-service fbkeyboard stop");
        g_fbkbd_visible = false;
    }
}

/* When entering menu: stop fbkeyboard service, grab its input device */
static void fbkbd_hide_for_menu(void)
{
    if (g_fbkbd_visible) fbkbd_service(false);
    fbkbd_grab_update();
}

/* When leaving menu: release fbkeyboard device grab */
static void fbkbd_restore_from_menu(void)
{
    fbkbd_grab_update();
    /* Do NOT auto-restart fbkeyboard — user controls it with HOME */
}

/* -------------------------------------------------------------------------
 * Screen blanking
 * ---------------------------------------------------------------------- */

static void screen_set_blank(bool blank)
{
    g_screen_blank = blank;
    write_sysfs(FB_BLANK, blank ? "1" : "0");
    touchscreen_update();
    log_info("screen %s", blank ? "blanked" : "unblanked");
}

/* -------------------------------------------------------------------------
 * Menu rendering
 * ---------------------------------------------------------------------- */

#define BOX_W 28   /* inner width of the menu box (characters) */

static void draw_hline(int l, int m, int r, int w)
{
    putchar(l);
    for (int i = 0; i < w; i++) putchar(m);
    putchar(r);
    putchar('\n');
}

static void menu_render(void)
{
    if (!g_menu_visible) return;

    fputs(ANSI_CLEAR, stdout);

    /* Title bar */
    fputs(ANSI_BOLD ANSI_CYAN, stdout);
    draw_hline(0xE2, 0x94, 0x80, BOX_W);   /* not unicode box — use ASCII */

    /* Use plain ASCII box drawing for maximum terminal compatibility */
    printf(ANSI_BOLD ANSI_CYAN "+-" ANSI_YELLOW " pmOS Menu " ANSI_CYAN);
    /* pad to width */
    int pad = BOX_W - 13; /* "pmOS Menu" visible width */
    for (int i = 0; i < pad; i++) putchar('-');
    puts("+");

    /* Breadcrumb */
    if (g_nav_depth > 0) {
        fputs(ANSI_CYAN "| " ANSI_DIM, stdout);
        for (int d = 0; d < g_nav_depth; d++) {
            printf("%s", g_nav_stack[d][g_nav_sel_stack[d]].label);
            if (d < g_nav_depth - 1) fputs(" > ", stdout);
        }
        /* pad rest */
        printf(ANSI_CYAN " |\n");
    }

    /* Separator */
    fputs(ANSI_CYAN "+", stdout);
    for (int i = 0; i < BOX_W; i++) putchar('-');
    puts("+");

    /* Items */
    for (int i = 0; i < g_current_len; i++) {
        bool sel = (i == g_current_sel);
        const char *arrow = g_current_menu[i].submenu ? " >" : "  ";
        if (sel)
            printf(ANSI_CYAN "| " ANSI_REV ANSI_BOLD "%-*s%s" ANSI_RESET ANSI_CYAN " |\n",
                   BOX_W - 4, g_current_menu[i].label, arrow);
        else
            printf(ANSI_CYAN "| " ANSI_RESET "%-*s%s" ANSI_CYAN " |\n",
                   BOX_W - 4, g_current_menu[i].label, arrow);
    }

    /* Footer */
    fputs(ANSI_CYAN "+", stdout);
    for (int i = 0; i < BOX_W; i++) putchar('-');
    puts("+");

    fputs(ANSI_DIM "VOL+/-:nav  PWR:select  BACK:back\n" ANSI_RESET, stdout);
    fflush(stdout);
}

/* -------------------------------------------------------------------------
 * Menu navigation
 * ---------------------------------------------------------------------- */

static void menu_push(MenuItem *items, int len)
{
    if (g_nav_depth >= MAX_DEPTH) return;
    /* save current level */
    g_nav_stack[g_nav_depth]     = g_current_menu;
    g_nav_len_stack[g_nav_depth] = g_current_len;
    g_nav_sel_stack[g_nav_depth] = g_current_sel;
    g_nav_depth++;

    g_current_menu = items;
    g_current_len  = len;
    g_current_sel  = 0;
}

static void menu_pop(void)
{
    if (g_nav_depth == 0) return;
    g_nav_depth--;
    g_current_menu = g_nav_stack[g_nav_depth];
    g_current_len  = g_nav_len_stack[g_nav_depth];
    g_current_sel  = g_nav_sel_stack[g_nav_depth];
}

static void menu_navigate(int delta)
{
    g_current_sel = (g_current_sel + delta + g_current_len) % g_current_len;
    menu_render();
}

static void menu_select(void)
{
    MenuItem *item = &g_current_menu[g_current_sel];
    if (item->submenu) {
        menu_push(item->submenu, item->submenu_len);
        menu_render();
    } else if (item->action) {
        item->action();
        menu_render(); /* re-draw after action (status may have changed) */
    }
}

static void menu_back(void)
{
    if (g_nav_depth == 0) return; /* already at root — ignore */
    menu_pop();
    menu_render();
}

/* -------------------------------------------------------------------------
 * Menu show / hide
 * ---------------------------------------------------------------------- */

static void menu_open(void);
static void menu_close(void);

static void menu_open(void)
{
    if (g_menu_visible) return;
    if (g_screen_blank) return; /* don't open menu on blank screen */
    g_menu_visible = true;

    /* Reset to root */
    g_nav_depth   = 0;
    g_current_sel = 0;

    fbkbd_hide_for_menu();
    touchscreen_update();
    cursor_hide();
    menu_render();
    log_info("menu opened");
}

static void menu_close(void)
{
    if (!g_menu_visible) return;
    g_menu_visible = false;

    fbkbd_restore_from_menu();
    touchscreen_update();

    /* Clear screen and show cursor */
    fputs(ANSI_CLEAR ANSI_RESET, stdout);
    fflush(stdout);
    cursor_show();
    log_info("menu closed");
}

/* -------------------------------------------------------------------------
 * Action callbacks
 * ---------------------------------------------------------------------- */

static void action_wifi_toggle(void)
{
    /* Use rfkill — available on pmOS */
    run_cmd("rfkill list wifi | grep -q 'Soft blocked: no' "
            "&& rfkill block wifi || rfkill unblock wifi");
    log_info("wifi toggled");
}

static void action_bt_toggle(void)
{
    run_cmd("rfkill list bluetooth | grep -q 'Soft blocked: no' "
            "&& rfkill block bluetooth || rfkill unblock bluetooth");
    log_info("bluetooth toggled");
}

static void action_reboot(void)
{
    menu_close();
    log_info("rebooting");
    run_cmd("reboot");
}

static void action_poweroff(void)
{
    menu_close();
    log_info("powering off");
    run_cmd("poweroff");
}

static void action_exit(void)
{
    menu_close();
    g_running = 0;
    log_info("exit requested");
}

/* -------------------------------------------------------------------------
 * Menu data — statically defined, data-driven
 * ---------------------------------------------------------------------- */

static MenuItem g_net_submenu[] = {
    { "Toggle WiFi",      action_wifi_toggle, NULL, 0 },
    { "Toggle Bluetooth", action_bt_toggle,   NULL, 0 },
};

static MenuItem g_power_submenu[] = {
    { "Reboot",    action_reboot,   NULL, 0 },
    { "Power Off", action_poweroff, NULL, 0 },
};

static MenuItem g_root_menu[] = {
    { "Networking", NULL, g_net_submenu,   2 },
    { "Power",      NULL, g_power_submenu, 2 },
    { "Exit Menu",  action_exit, NULL, 0 },
};

#define ROOT_MENU_LEN ((int)(sizeof(g_root_menu) / sizeof(g_root_menu[0])))

/* -------------------------------------------------------------------------
 * Input event handling
 * ---------------------------------------------------------------------- */

/*
 * State machine transitions triggered by key-press events.
 *
 *  Global states
 *    SCREEN_BLANK  : screen is blank
 *    MENU_OPEN     : menu is visible
 *    NORMAL        : normal terminal mode
 *
 *  Events (key presses only — value==1):
 *    POWER    : NORMAL  → toggle blank | BLANK → unblank | MENU → ignored
 *    MENU     : NORMAL  → open menu   | MENU  → close menu
 *    BACK     : MENU    → back/close  | otherwise → ignored
 *    HOME     : NORMAL  → toggle fbkbd
 *    VOL+     : MENU    → navigate up
 *    VOL-     : MENU    → navigate down
 */

static void handle_key(int code, int value)
{
    /* Only react to key-down events (value == 1).
     * Ignore key-up (0) and key-repeat (2). */
    if (value != 1) return;

    switch (code) {

    case KEY_POWER_CODE:
        if (g_menu_visible) {
            menu_select();                  /* POWER = select when in menu */
        } else if (g_screen_blank) {
            screen_set_blank(false);
        } else {
            screen_set_blank(true);
        }
        break;

    case KEY_MENU_CODE:
        if (g_screen_blank) break;          /* screen must be on */
        if (g_menu_visible)
            menu_close();
        else
            menu_open();
        break;

    case KEY_BACK_CODE:
        if (!g_menu_visible) break;
        if (g_nav_depth == 0)
            menu_close();
        else
            menu_back();
        break;

    case KEY_HOME_CODE:
        if (g_menu_visible) break;          /* ignored in menu */
        if (g_screen_blank) break;          /* screen must be on */
        if (g_fbkbd_visible)
            fbkbd_service(false);
        else
            fbkbd_service(true);
        break;

    case KEY_VOLUMEUP_CODE:
        if (!g_menu_visible) break;
        menu_navigate(-1);                  /* up = previous item */
        break;

    case KEY_VOLUMEDOWN_CODE:
        if (!g_menu_visible) break;
        menu_navigate(+1);                  /* down = next item */
        break;

    default:
        break;
    }
}

static void process_event(const struct input_event *ev)
{
    if (ev->type == EV_KEY)
        handle_key(ev->code, ev->value);
}

static void read_events(int fd)
{
    struct input_event buf[32];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0) {
        if (errno != EINTR && errno != EAGAIN)
            log_err("read event");
        return;
    }
    int count = (int)(n / sizeof(struct input_event));
    for (int i = 0; i < count; i++)
        process_event(&buf[i]);
}

/* -------------------------------------------------------------------------
 * Device open helpers
 * ---------------------------------------------------------------------- */

static int open_device(const char *path, bool grab)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        log_err("open %s", path);
        return -1;
    }
    if (grab) {
        if (grab_device(fd, true) < 0)
            log_err("EVIOCGRAB %s", path);
    }
    return fd;
}

static void epoll_add(int epfd, int fd)
{
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0)
        log_err("epoll_ctl add");
}

/* -------------------------------------------------------------------------
 * Signal handling
 * ---------------------------------------------------------------------- */

static void sig_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* -------------------------------------------------------------------------
 * Cleanup
 * ---------------------------------------------------------------------- */

static void cleanup(void)
{
    /* Release grabs before closing */
    if (g_fd_gpio     >= 0) { grab_device(g_fd_gpio,     false); close(g_fd_gpio);     }
    if (g_fd_touchkey >= 0) { grab_device(g_fd_touchkey, false); close(g_fd_touchkey); }
    if (g_fd_touch    >= 0) { grab_device(g_fd_touch,    false); close(g_fd_touch);    }
    if (g_fd_fbkbd    >= 0) { grab_device(g_fd_fbkbd,    false); close(g_fd_fbkbd);    }
    if (g_epfd        >= 0) close(g_epfd);

    /* Restore screen and terminal */
    screen_set_blank(false);
    fputs(ANSI_CLEAR ANSI_RESET, stdout);
    fflush(stdout);
    cursor_show();
    terminal_restore();
    log_info("cleanup done");
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(void)
{
    /* Install signal handlers */
    struct sigaction sa = { .sa_handler = sig_handler };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);

    /* Switch terminal to raw mode */
    terminal_raw();
    cursor_hide();
    fputs(ANSI_CLEAR, stdout);
    fflush(stdout);

    /* Open input devices.
     * gpio-keys and touchkey are grabbed immediately — we are the sole consumer.
     * Touchscreen is opened but not yet grabbed (normal mode, screen on).
     * fbkeyboard is opened but not yet grabbed. */
    g_fd_gpio     = open_device(DEV_GPIO,     true);
    g_fd_touchkey = open_device(DEV_TOUCHKEY, true);
    g_fd_touch    = open_device(DEV_TOUCH,    false);  /* grab managed dynamically */
    g_fd_fbkbd    = open_device(DEV_FBKBD,    false);  /* grab managed dynamically */

    if (g_fd_gpio < 0 || g_fd_touchkey < 0) {
        log_info("Cannot open required input devices — aborting.");
        cleanup();
        return EXIT_FAILURE;
    }

    /* epoll setup */
    g_epfd = epoll_create1(EPOLL_CLOEXEC);
    if (g_epfd < 0) { log_err("epoll_create1"); cleanup(); return EXIT_FAILURE; }

    epoll_add(g_epfd, g_fd_gpio);
    epoll_add(g_epfd, g_fd_touchkey);
    /* We do not add touch/fbkbd to epoll — we grab them but never read them here */

    /* Initialise menu root */
    g_current_menu = g_root_menu;
    g_current_len  = ROOT_MENU_LEN;
    g_current_sel  = 0;

    log_info("started — MENU button opens/closes menu");

    /* Main event loop */
    struct epoll_event events[8];
    while (g_running) {
        int n = epoll_wait(g_epfd, events, 8, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            log_err("epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++)
            read_events(events[i].data.fd);
    }

    cleanup();
    return EXIT_SUCCESS;
}
