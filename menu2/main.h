/*
 * main.h — shared types, constants, and cross-file declarations
 *
 * postmarketOS hardware-button menu — Samsung Galaxy S2 GT-I9100
 */
#ifndef MAIN_H
#define MAIN_H

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>

/* ── Device paths ─────────────────────────────────────────────────────────── */
#define DEV_GPIO        "/dev/input/event0"
#define DEV_TOUCHKEY    "/dev/input/event1"
#define DEV_TOUCH       "/dev/input/event2"
#define DEV_FBKBD       "/dev/input/event3"
#define FB_BLANK_PATH      "/sys/class/graphics/fb0/blank"
#define BRIGHTNESS_PATH    "/sys/class/backlight/spi3.0/brightness"
#define BRIGHTNESS_MIN     0
#define BRIGHTNESS_MAX     24
#define BRIGHTNESS_DEFAULT 12

/* ── Key codes ────────────────────────────────────────────────────────────── */
#define KEY_VOLUMEUP_CODE   115
#define KEY_VOLUMEDOWN_CODE 114
#define KEY_POWER_CODE      116
#define KEY_HOME_CODE       352
#define KEY_MENU_CODE       139
#define KEY_BACK_CODE       158

/* ── ANSI escape helpers ──────────────────────────────────────────────────── */
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
    STATE_IDLE,   /* Screen on, terminal visible, no menu  */
    STATE_MENU,       /* Menu visible and navigable            */
    STATE_BRIGHTNESS, /* Brightness adjustment overlay          */
    STATE_ANY,        /* Wildcard — matches any state in table */
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
 * Menu types
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct Menu Menu;
typedef void        (*ActionFn)(void);
typedef const char *(*StatusFn)(void);  /* returns "ON"/"OFF"/NULL at render time */

typedef struct {
    const char *label;
    ActionFn    action;   /* NULL for submenu items                */
    Menu       *submenu;  /* NULL for leaf items                   */
    StatusFn    status;   /* NULL = no badge; else called at draw  */
} MenuItem;

struct Menu {
    const char *title;
    MenuItem   *items;
    uint8_t     count;
    const Menu *parent;  /* NULL at root; set at runtime for submenus */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Global state — defined in main.c, used across all translation units
 * ═══════════════════════════════════════════════════════════════════════════ */
extern volatile sig_atomic_t g_running;
extern AppState    g_state;
extern bool        g_screen_blank;
extern bool        g_fbkbd_on;
extern const Menu *g_menu;
extern uint8_t     g_sel;
extern int         g_brightness;

/* File descriptors (system.c owns open/close, all TUs may read) */
extern int g_fd_gpio;
extern int g_fd_touchkey;
extern int g_fd_touch;
extern int g_fd_fbkbd;
extern int g_fd_fb;

/* ── Menu root (defined in menu.c) ───────────────────────────────────────── */
extern const Menu g_root_menu;
extern Menu       g_net_menu;
extern Menu       g_pwr_menu;

/* ═══════════════════════════════════════════════════════════════════════════
 * system.c — public API
 * ═══════════════════════════════════════════════════════════════════════════ */
void log_info(const char *fmt, ...);
void log_err (const char *fmt, ...);

void terminal_raw    (void);
void terminal_restore(void);

void fb_set_blank       (bool blank);
int  brightness_read    (void);
void brightness_write   (int level);
int  run_cmd      (char *const argv[]);
void grab         (int fd, bool on);
void update_grabs (void);
void fbkbd_set    (bool start);
int  open_dev     (const char *path, bool grab_now);

/* ═══════════════════════════════════════════════════════════════════════════
 * menu.c — public API
 * ═══════════════════════════════════════════════════════════════════════════ */
void render(void);

/* th_close_menu is also needed by menu.c's action_exit_menu;
 * declared here so menu.c can call it without knowing FSM internals. */
void th_close_menu(void);
void th_brightness_enter(void);  /* called by action_brightness in menu.c */

#endif /* MAIN_H */
