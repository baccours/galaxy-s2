/*
 * menu.c — menu tree, leaf actions, on_enter hooks, and rendering
 *
 * To add a plain submenu:  add items[], a Menu with on_enter=NULL, link
 *                          parent in main(), add a MenuItem pointing at it.
 * To add an overlay menu:  add a Menu with count=0, items=NULL, an on_enter
 *                          that sets g_state and calls render().  Add a
 *                          MenuItem pointing at it.  No other file changes.
 */

#include "button-spy.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Networking — status cache + on_enter hook
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool g_wifi_on = false;
static bool g_bt_on   = false;

/* Query live state; called once each time the Networking submenu is entered. */
static void net_on_enter(void)
{
    char *const wifi_chk[] = { WIFI_TOGGLE_SCRIPT, "status", NULL };
    char *const bt_chk[]   = { BT_TOGGLE_SCRIPT,   "status", NULL };
    g_wifi_on = (run_cmd(wifi_chk) == 0);
    g_bt_on   = (run_cmd(bt_chk)   == 0);
}

static const char *status_wifi(void) { return g_wifi_on ? "ON" : "OFF"; }
static const char *status_bt  (void) { return g_bt_on   ? "ON" : "OFF"; }

static void action_wifi_toggle(void)
{
    char *const args[] = {
        (char *)WIFI_TOGGLE_SCRIPT,
        (char *)(g_wifi_on ? "off" : "on"),
        NULL
    };
    g_wifi_on = !g_wifi_on;
    run_cmd(args);
}

static void action_bt_toggle(void)
{
    char *const args[] = {
        (char *)BT_TOGGLE_SCRIPT,
        (char *)(g_bt_on ? "off" : "on"),
        NULL
    };
    g_bt_on = !g_bt_on;
    run_cmd(args);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Brightness — item-less overlay menu
 * ═══════════════════════════════════════════════════════════════════════════ */

static void brightness_on_enter(void)
{
    g_brightness = brightness_read();
    g_state      = STATE_BRIGHTNESS;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Battery — item-less overlay menu
 * ═══════════════════════════════════════════════════════════════════════════ */

static void battery_on_enter(void)
{
    battery_read();
    g_state = STATE_BATTERY;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Power — leaf actions
 * ═══════════════════════════════════════════════════════════════════════════ */

static void action_reboot  (void) { char *const a[] = { "reboot",   NULL }; run_cmd(a); }
static void action_poweroff(void) { char *const a[] = { "poweroff", NULL }; run_cmd(a); }

/* ═══════════════════════════════════════════════════════════════════════════
 * Menu tree
 * ═══════════════════════════════════════════════════════════════════════════ */

static MenuItem g_net_items[] = {
    { "Toggle WiFi",      action_wifi_toggle, NULL, status_wifi },
    { "Toggle Bluetooth", action_bt_toggle,   NULL, status_bt   },
};
Menu g_net_menu = { "Networking", g_net_items, ARRAY_SIZE(g_net_items), NULL, net_on_enter };

static MenuItem g_pwr_items[] = {
    { "Reboot",    action_reboot,   NULL, NULL },
    { "Power Off", action_poweroff, NULL, NULL },
};
Menu g_pwr_menu = { "Power", g_pwr_items, ARRAY_SIZE(g_pwr_items), NULL, NULL };

Menu g_brightness_menu = { "Brightness",     NULL, 0, NULL, brightness_on_enter };
Menu g_battery_menu    = { "Battery Status", NULL, 0, NULL, battery_on_enter    };

static MenuItem g_root_items[] = {
    { "Networking",     NULL,          &g_net_menu,       NULL },
    { "Power",          NULL,          &g_pwr_menu,       NULL },
    { "Brightness",     NULL,          &g_brightness_menu, NULL },
    { "Battery Status", NULL,          &g_battery_menu,   NULL },
    { "Exit Menu",      th_close_menu, NULL,              NULL },
};
const Menu g_root_menu = { "pmOS  GT-I9100", g_root_items, ARRAY_SIZE(g_root_items), NULL, NULL };

/* ── Runtime wiring — called from main() ──────────────────────────────────
 * initialisers cannot forward-reference objects in the same translation unit.
 */

void menu_init(void)
{
    g_net_menu.parent        = &g_root_menu;
    g_pwr_menu.parent        = &g_root_menu;
    g_brightness_menu.parent = &g_root_menu;
    g_battery_menu.parent    = &g_root_menu;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Rendering — single entry point
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── Box-drawing helpers ──────────────────────────────────────────────────── */

static void draw_hline(void)
{
    fputc('+', g_tty);
    for (int i = 0; i < BOX_W; i++) fputc('-', g_tty);
    fputs("+\r\n", g_tty);
}

/* Print text left-aligned inside a box row; pads to fill BOX_W - 2. */
static void box_title_row(const char *pre_esc, const char *text)
{
    fputs("| ", g_tty);
    if (pre_esc) fputs(pre_esc, g_tty);
    int vis = (int)strlen(text);
    fputs(text, g_tty);
    for (int i = vis; i < BOX_W - 2; i++) fputc(' ', g_tty);
    fputs(T_CYAN " |\r\n", g_tty);
}

/* ── STATE_MENU ───────────────────────────────────────────────────────────── */

static void render_menu(void)
{
    /* Breadcrumb: walk from current node to root, collect, print root-first. */
    const Menu *path[16];
    int depth = 0;
    for (const Menu *m = g_menu; m && depth < 16; m = m->parent)
        path[depth++] = m;

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

    fputs("| " T_YELLOW, g_tty);
    fputs(crumb, g_tty);
    for (int i = vis; i < BOX_W - 2; i++) fputc(' ', g_tty);
    fputs(T_CYAN " |\r\n", g_tty);

    draw_hline();

    for (uint8_t i = 0; i < g_menu->count; i++) {
        bool        sel     = (i == g_sel);
        bool        has_sub = (g_menu->items[i].submenu != NULL);
        const char *badge   = g_menu->items[i].status
                              ? g_menu->items[i].status() : NULL;
        const char *arrow   = has_sub ? ">" : " ";
        char        badge_buf[8] = "  ";
        if (badge) snprintf(badge_buf, sizeof(badge_buf), "%-3s", badge);

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

/* ── STATE_BRIGHTNESS ─────────────────────────────────────────────────────── */

static void render_brightness(void)
{
    fputs(T_BOLD T_CYAN, g_tty);
    draw_hline();
    box_title_row(T_YELLOW, "Brightness");
    draw_hline();

    char level_buf[16];
    int level_vis = snprintf(level_buf, sizeof(level_buf),
                             " %2d / %-2d ", g_brightness, BRIGHTNESS_MAX);
    fprintf(g_tty, "| " T_RESET "%s" T_CYAN, level_buf);

    int bar_vis = BRIGHTNESS_MAX + 1 + 2;
    fputs(T_BOLD "[", g_tty);
    for (int i = 0; i <= BRIGHTNESS_MAX; i++) {
        if (i == g_brightness) fputs(T_DIM, g_tty);
        fputc(i < g_brightness ? '#' : '-', g_tty);
    }
    fputs(T_RESET T_CYAN "]", g_tty);

    int pad = BOX_W - 2 - level_vis - bar_vis;
    for (int i = 0; i < pad; i++) fputc(' ', g_tty);
    fputs(" |\r\n", g_tty);

    draw_hline();
    fputs(T_DIM "VOL+/-: adjust   PWR/BACK: done\r\n" T_RESET, g_tty);
}

/* ── STATE_BATTERY ────────────────────────────────────────────────────────── */

static void render_battery(void)
{
    fputs(T_BOLD T_CYAN, g_tty);
    draw_hline();
    box_title_row(T_YELLOW, "Battery Status");
    draw_hline();

    if (g_batt_nlines == 0) {
        const char *msg = "  (no data)";
        fputs("| " T_RESET, g_tty);
        fputs(msg, g_tty);
        int pad = BOX_W - 2 - (int)strlen(msg);
        for (int i = 0; i < pad; i++) fputc(' ', g_tty);
        fputs(T_CYAN " |\r\n", g_tty);
    } else {
        for (int i = 0; i < g_batt_nlines; i++) {
            int vis = (int)strlen(g_batt_lines[i]);
            fputs("| " T_RESET, g_tty);
            fputs(g_batt_lines[i], g_tty);
            for (int j = vis; j < BOX_W - 2; j++) fputc(' ', g_tty);
            fputs(T_CYAN " |\r\n", g_tty);
        }
    }

    draw_hline();
    fputs(T_DIM "PWR/BACK: back to menu\r\n" T_RESET, g_tty);
}

/* ── Unified render ───────────────────────────────────────────────────────── */

void render(void)
{
    fputs(T_CLEAR, g_tty);
    switch (g_state) {
        case STATE_MENU:       render_menu();       break;
        case STATE_BRIGHTNESS: render_brightness(); break;
        case STATE_BATTERY:    render_battery();    break;
        default:                                    break;
    }
    fflush(g_tty);
}
