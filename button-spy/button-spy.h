/*
 * button-spy.h — shared types, constants, and cross-file declarations
 *
 * postmarketOS hardware-button menu — Samsung Galaxy S2 GT-I9100
 */
#ifndef BTNSPY_H
#define BTNSPY_H

#define _GNU_SOURCE
#include <linux/input.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* ── utils ────────────────────────────────────────────────────────────────── */
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/* ── Device paths ─────────────────────────────────────────────────────────── */
#define DEV_GPIO        "/dev/input/event0"
#define DEV_TOUCHKEY    "/dev/input/event1"
#define DEV_TOUCH       "/dev/input/event2"
#define DEV_TTY              "/dev/tty1"
#define BT_TOGGLE_SCRIPT     "/usr/local/bin/bluetooth-toggle"
#define FB_BLANK_PATH   "/sys/class/graphics/fb0/blank"
#define BRIGHTNESS_PATH "/sys/class/backlight/spi3.0/brightness"
#define BRIGHTNESS_MIN  0
#define BRIGHTNESS_MAX  24
#define BRIGHTNESS_DEFAULT 12

/* ── ANSI escape helpers ──────────────────────────────────────────────────── */
// Blanking : 0 disable, restore to 1 min
#define T_DISABLE_BLANK "\033[9;0]"
#define T_RESTORE_BLANK "\033[9;1]"

#define T_CLEAR  "\033[H\033[J"
#define T_RESET  "\033[0m"
#define T_BOLD   "\033[1m"
#define T_DIM    "\033[2m"
#define T_REV    "\033[7m"
#define T_CYAN   "\033[36m"
#define T_YELLOW "\033[33m"
#define T_HIDE   "\033[?25l"
#define T_SHOW   "\033[?25h"

#define BOX_W 50   /* printable width of the menu box interior */

/* ── FSM states & events ─────────────────────────────────────────────────── */
typedef enum {
    STATE_IDLE,       /* screen on, no menu                    */
    STATE_MENU,       /* menu visible and navigable            */
    STATE_BRIGHTNESS, /* brightness adjustment overlay         */
    STATE_ANY,        /* wildcard — matches any state in table */
} AppState;

/* ── Menu types ───────────────────────────────────────────────────────────── */
typedef struct Menu Menu;
typedef void        (*ActionFn)(void);
typedef const char *(*StatusFn)(void);   /* returns "ON"/"OFF"/NULL at render time */

typedef struct {
    const char *label;
    ActionFn    action;   /* NULL for submenu nodes */
    Menu       *submenu;  /* NULL for leaf items    */
    StatusFn    status;   /* NULL = no badge        */
} MenuItem;

struct Menu {
    const char *title;
    MenuItem   *items;
    uint8_t     count;
    const Menu *parent;   /* NULL at root; linked at runtime for submenus */
};

/* ── Global state — defined in system.c ──────────────────────────────────── */
extern volatile sig_atomic_t g_running;
extern AppState    g_state;
extern bool        g_screen_blank;
extern bool        g_fbkbd_on;
extern const Menu *g_menu;
extern uint8_t     g_sel;
extern int         g_brightness;

extern int   g_fd_gpio;
extern int   g_fd_touchkey;
extern int   g_fd_touch;
extern int   g_fd_fb;
extern FILE *g_tty;   /* DEV_TTY — all display output goes here */

/* ── Menu tree — defined in menu.c ───────────────────────────────────────── */
extern const Menu g_root_menu;
extern Menu       g_net_menu;
extern Menu       g_pwr_menu;

/* ── system.c API ────────────────────────────────────────────────────────── */
void log_info(const char *fmt, ...);
void log_err (const char *fmt, ...);

bool terminal_open   (void);
void terminal_raw    (void);
bool terminal_restore(void);
void terminal_close  (void);

void fb_set_blank    (bool blank);
int  brightness_read (void);
void brightness_write(int level);
int  run_cmd         (char *const argv[]);
void grab            (int fd, bool on);
void update_grabs    (void);
void fbkbd_set       (bool start);
int  open_dev        (const char *path, bool grab_now);

/* ── menu.c API ──────────────────────────────────────────────────────────── */
void render(void);
void net_invalidate(void);  /* mark network status cache stale */

/* Declared here so menu.c's action callbacks can trigger FSM transitions
 * without duplicating state-mutation logic. */
void th_close_menu      (void);
void th_brightness_enter(void);

#endif /* BTNSPY_H */
