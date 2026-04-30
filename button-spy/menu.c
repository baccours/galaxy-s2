/*
 * menu.c — menu tree, leaf actions, and rendering
 *
 * To add a menu entry: add an action function and a row in the relevant
 * items[] array. No other file needs to change.
 */

#include "button-spy.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Networking status cache
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool g_wifi_on    = false;
static bool g_bt_on      = false;
static bool g_net_dirty  = true;   /* set true to force re-query on next render */

static void net_query_status(void)
{
    if (!g_net_dirty) return;
    char *const wifi_chk[] = { WIFI_TOGGLE_SCRIPT, "status", NULL };
    char *const bt_chk[] = { BT_TOGGLE_SCRIPT, "status", NULL };
    g_wifi_on  = (run_cmd((char *const *)wifi_chk) == 0);
    g_bt_on    = (run_cmd((char *const *)bt_chk)   == 0);
    g_net_dirty = false;
}

static const char *status_wifi(void) { net_query_status(); return g_wifi_on ? "ON" : "OFF"; }
static const char *status_bt  (void) { net_query_status(); return g_bt_on   ? "ON" : "OFF"; }

void net_invalidate(void) { g_net_dirty = true; }

/* ═══════════════════════════════════════════════════════════════════════════
 * Leaf action callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */

static void action_wifi_toggle(void)
{
    char *const off[] = { WIFI_TOGGLE_SCRIPT, "off", NULL };
    char *const on[]  = { WIFI_TOGGLE_SCRIPT, "on",  NULL };
    run_cmd(g_wifi_on ? off : on);
    g_wifi_on = !g_wifi_on;
}

static void action_bt_toggle(void)
{
    char *const off[] = { BT_TOGGLE_SCRIPT, "off", NULL };
    char *const on[]  = { BT_TOGGLE_SCRIPT, "on",  NULL };
    run_cmd(g_bt_on ? off : on);
    g_bt_on = !g_bt_on;
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

static void action_brightness(void) { th_brightness_enter(); }
static void action_exit_menu(void)  { th_close_menu(); }

/* ═══════════════════════════════════════════════════════════════════════════
 * Menu tree
 *
 * Edit only these tables to add/remove/reorder entries.
 * Parent pointers are set at runtime in main() — C static initialisers
 * cannot forward-reference an object defined later in the same TU.
 * ═══════════════════════════════════════════════════════════════════════════ */

static MenuItem g_net_items[] = {
    { "Toggle WiFi",      action_wifi_toggle, NULL, status_wifi },
    { "Toggle Bluetooth", action_bt_toggle,   NULL, status_bt   },
};
Menu g_net_menu = { "Networking", g_net_items, ARRAY_SIZE(g_net_items), NULL };

static MenuItem g_pwr_items[] = {
    { "Reboot",    action_reboot,   NULL, NULL },
    { "Power Off", action_poweroff, NULL, NULL },
};
Menu g_pwr_menu = { "Power", g_pwr_items, ARRAY_SIZE(g_pwr_items), NULL };

static MenuItem g_root_items[] = {
    { "Networking",  NULL,              &g_net_menu, NULL },
    { "Power",       NULL,              &g_pwr_menu, NULL },
    { "Brightness",  action_brightness, NULL,        NULL },
    { "Exit Menu",   action_exit_menu,  NULL,        NULL },
};
const Menu g_root_menu = { "pmOS  GT-I9100", g_root_items, ARRAY_SIZE(g_root_items), NULL };

/* ═══════════════════════════════════════════════════════════════════════════
 * Rendering — single entry point
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── Box-drawing helper ───────────────────────────────────────────────────── */

static void draw_hline(void)
{
    fputc('+', g_tty);
    for (int i = 0; i < BOX_W; i++) fputc('-', g_tty);
    fputs("+\r\n", g_tty);
}

/* Print text left-aligned inside a box row; pads to fill BOX_W - 2.
 * Returns the number of visible characters written (excluding ANSI codes). */
static int box_title_row(const char *pre_esc, const char *text, const char *post_esc)
{
    fputs("| ", g_tty);
    if (pre_esc)  fputs(pre_esc,  g_tty);
    int vis = fputs(text, g_tty) >= 0 ? (int)strlen(text) : 0;
    if (post_esc) fputs(post_esc, g_tty);
    for (int i = vis; i < BOX_W - 2; i++) fputc(' ', g_tty);
    fputs(T_CYAN " |\r\n", g_tty);
    return vis;
}

static void render_menu(void)
{
    /* Breadcrumb: walk from current node to root via parent pointers,
     * collect the path, then print root-first. */
    const Menu *path[16];
    int depth = 0;
    for (const Menu *m = g_menu; m && depth < 16; m = m->parent)
        path[depth++] = m;

    /* Build the breadcrumb string so we can measure its visible length
     * precisely — ANSI escapes must not be counted. */
    char crumb[128] = "";
    int  vis        = 0;
    for (int i = depth - 1; i >= 0; i--) {
        const char *sep = (i > 0) ? " > " : "";
        vis += (int)(strlen(path[i]->title) + strlen(sep));
        strncat(crumb, path[i]->title, sizeof(crumb) - strlen(crumb) - 1);
        strncat(crumb, sep,            sizeof(crumb) - strlen(crumb) - 1);
    }

    fputs(T_BOLD T_CYAN, g_tty);
    draw_hline();

    /* Breadcrumb row */
    fputs("| " T_YELLOW, g_tty);
    fputs(crumb, g_tty);
    for (int i = vis; i < BOX_W - 2; i++) fputc(' ', g_tty);
    fputs(T_CYAN " |\r\n", g_tty);

    draw_hline();

    /* Items */
    for (uint8_t i = 0; i < g_menu->count; i++) {
        bool        sel     = (i == g_sel);
        bool        has_sub = (g_menu->items[i].submenu != NULL);
        const char *badge   = g_menu->items[i].status
                              ? g_menu->items[i].status() : NULL;
        const char *arrow   = has_sub ? ">" : " ";
        char        badge_buf[8] = "  ";   /* two spaces when no badge */
        if (badge)
            snprintf(badge_buf, sizeof(badge_buf), "%-3s", badge);

        if (sel)
            fprintf(g_tty, "| " T_REV T_BOLD "%-*s%s%s" T_RESET T_CYAN " |\r\n",
                    BOX_W - 8, g_menu->items[i].label, badge_buf, arrow);
        else
            fprintf(g_tty, "| " T_RESET "%-*s%s%s" T_CYAN " |\r\n",
                    BOX_W - 8, g_menu->items[i].label, badge_buf, arrow);
    }

    draw_hline();
    fputs(T_DIM "VOL+/-: navigate   PWR: select   BACK: back\r\n" T_RESET, g_tty);
}

static void render_brightness(void)
{
    fputs(T_BOLD T_CYAN, g_tty);
    draw_hline();

    box_title_row(T_YELLOW, "Brightness", NULL);

    draw_hline();

    /* Level fraction — measure visible chars written */
    char level_buf[16];
    int level_vis = snprintf(level_buf, sizeof(level_buf),
                             " %2d / %-2d ", g_brightness, BRIGHTNESS_MAX);
    fprintf(g_tty, "| " T_RESET "%s" T_CYAN, level_buf);

    /* Bar: filled in bold, empty in dim */
    int bar_vis = BRIGHTNESS_MAX + 1 + 2; /* chars inside [] plus the brackets */
    fputs(T_BOLD "[", g_tty);
    for (int i = 0; i <= BRIGHTNESS_MAX; i++) {
        if (i == g_brightness) fputs(T_DIM, g_tty);
        fputc(i < g_brightness ? '#' : '-', g_tty);
    }
    fputs(T_RESET T_CYAN "]", g_tty);

    /* Pad: BOX_W - 2 borders - "| " prefix - level - bar */
    int pad = BOX_W - 2 - level_vis - bar_vis;
    for (int i = 0; i < pad; i++) fputc(' ', g_tty);
    fputs(" |\r\n", g_tty);

    draw_hline();
    fputs(T_DIM "VOL+/-: adjust   PWR/BACK: done\r\n" T_RESET, g_tty);
}

/* Unified render — always clears, then delegates on g_state */
void render(void)
{
    fputs(T_CLEAR, g_tty);
    switch (g_state) {
        case STATE_MENU:       render_menu();       break;
        case STATE_BRIGHTNESS: render_brightness(); break;
        default:                                    break;
    }
    fflush(g_tty);
}
