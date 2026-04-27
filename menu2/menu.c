/*
 * menu.c — menu tree, leaf actions, and rendering
 *
 * To add a menu entry: add an action function and a row in the relevant
 * items[] array. No other file needs to change.
 */

#include "main.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Networking status cache
 *
 * Populated once via net_query_status() when entering the Networking submenu.
 * Updated locally after each toggle — no re-query needed.
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool g_wifi_on    = false;
static bool g_bt_on      = false;
static bool g_net_dirty  = true;   /* set true to force re-query on next render */

static void net_query_status(void)
{
    if (!g_net_dirty) return;
    char *const wifi_chk[] = { "sh", "-c",
        "rfkill list wifi | grep -q 'Soft blocked: no'", NULL };
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
    char *const off[] = { "rfkill", "block",   "wifi", NULL };
    char *const on[]  = { "rfkill", "unblock", "wifi", NULL };
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
 * Menu tree — edit only these tables to extend the menu
 * ═══════════════════════════════════════════════════════════════════════════ */

static MenuItem g_net_items[] = {
    { "Toggle WiFi",      action_wifi_toggle, NULL, status_wifi },
    { "Toggle Bluetooth", action_bt_toggle,   NULL, status_bt   },
};
Menu g_net_menu = { "Networking", g_net_items, 2, NULL };

static MenuItem g_pwr_items[] = {
    { "Reboot",    action_reboot,   NULL, NULL },
    { "Power Off", action_poweroff, NULL, NULL },
};
Menu g_pwr_menu = { "Power", g_pwr_items, 2, NULL };

static MenuItem g_root_items[] = {
    { "Networking", NULL, &g_net_menu, NULL },
    { "Power",      NULL, &g_pwr_menu, NULL },
    { "Brightness", action_brightness, NULL, NULL },
    { "Exit Menu",  action_exit_menu,  NULL, NULL },
};
const Menu g_root_menu = { "pmOS  GT-I9100", g_root_items, 4, NULL };

/* ═══════════════════════════════════════════════════════════════════════════
 * Rendering
 * ═══════════════════════════════════════════════════════════════════════════ */

static void render_menu(void)
{
    /* Breadcrumb: walk from current node to root, then print root-first */
    const Menu *path[16];
    int depth = 0;
    for (const Menu *m = g_menu; m && depth < 16; m = m->parent)
        path[depth++] = m;

    fputs(T_BOLD T_CYAN, g_tty);
    fputs("+", g_tty);
    for (int i = 0; i < BOX_W; i++) fputc('-', g_tty);
    fputs("+\r\n", g_tty);

    fputs("| " T_YELLOW, g_tty);
    int used = 0;
    for (int i = depth - 1; i >= 0; i--)
        used += fprintf(g_tty, "%s%s", path[i]->title, i > 0 ? " > " : "");
    for (int i = used; i < BOX_W - 2; i++) fputc(' ', g_tty);
    fputs(T_CYAN " |\r\n", g_tty);

    fputs("+", g_tty);
    for (int i = 0; i < BOX_W; i++) fputc('-', g_tty);
    fputs("+\r\n", g_tty);

    for (uint8_t i = 0; i < g_menu->count; i++) {
        bool        sel     = (i == g_sel);
        bool        has_sub = (g_menu->items[i].submenu != NULL);
        const char *badge   = g_menu->items[i].status
                              ? g_menu->items[i].status() : NULL;
        const char *arrow   = has_sub ? ">" : " ";
        char        badge_buf[8] = "   ";
        if (badge) snprintf(badge_buf, sizeof(badge_buf), "%-3s", badge);

        if (sel)
            fprintf(g_tty, "| " T_REV T_BOLD "%-*s%s%s" T_RESET T_CYAN " |\r\n",
                    BOX_W - 8, g_menu->items[i].label, badge_buf, arrow);
        else
            fprintf(g_tty, "| " T_RESET "%-*s%s%s" T_CYAN " |\r\n",
                    BOX_W - 8, g_menu->items[i].label, badge_buf, arrow);
    }

    fputs(T_CYAN "+", g_tty);
    for (int i = 0; i < BOX_W; i++) fputc('-', g_tty);
    fputs("+\r\n", g_tty);
    fputs(T_DIM "VOL+/-: navigate   PWR: select   BACK: back\r\n" T_RESET, g_tty);
}

static void render_brightness(void)
{
    fputs(T_BOLD T_CYAN, g_tty);
    fputs("+", g_tty);
    for (int i = 0; i < BOX_W; i++) fputc('-', g_tty);
    fputs("+\r\n", g_tty);

    fputs("| " T_YELLOW, g_tty);
    int tlen = fprintf(g_tty, "Brightness");
    for (int i = tlen; i < BOX_W - 2; i++) fputc(' ', g_tty);
    fputs(T_CYAN " |\r\n", g_tty);

    fputs("+", g_tty);
    for (int i = 0; i < BOX_W; i++) fputc('-', g_tty);
    fputs("+\r\n", g_tty);

    fprintf(g_tty, "| " T_RESET " %2d / %-2d " T_CYAN, g_brightness, BRIGHTNESS_MAX);
    fputs(T_BOLD "[", g_tty);
    for (int i = 0; i <= BRIGHTNESS_MAX; i++) {
        if (i == g_brightness) fputs(T_DIM, g_tty);
        fputc(i < g_brightness ? '#' : '-', g_tty);
    }
    fputs(T_RESET T_CYAN "]", g_tty);
    for (int i = 0; i < BOX_W - 39; i++) fputc(' ', g_tty);
    fputs(" |\r\n", g_tty);

    fputs(T_CYAN "+", g_tty);
    for (int i = 0; i < BOX_W; i++) fputc('-', g_tty);
    fputs("+\r\n", g_tty);
    fputs(T_DIM "VOL+/-: adjust   PWR/BACK: done\r\n" T_RESET, g_tty);
}

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
