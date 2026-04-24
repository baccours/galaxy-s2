/*
 * menu.c — menu tree, leaf actions, and rendering
 *
 * Owns: MenuItem/Menu static data, action callbacks, render().
 * To add a menu entry: add an action function and a row in the relevant
 * *_items[] array. No other file needs to change.
 *
 * Depends on: system.c (run_cmd, fb_set_blank, fbkbd_set, log_*)
 *             pmenu.c  (th_close_menu — exit-menu action)
 */

#include "pmenu.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ── rfkill status helpers ────────────────────────────────────────────────── */

/*
 * Returns "ON" if the given rfkill type is unblocked, "OFF" if blocked.
 * Reads /sys/class/rfkill/rfkillN/type and soft_blocked to avoid forking
 * at render time — render is called on every keypress so we keep it cheap.
 * Falls back to "?" if sysfs is unreadable (e.g. module not loaded).
 */
static const char *rfkill_status(const char *type)
{
    char path[64], buf[32];
    for (int i = 0; i < 16; i++) {
        snprintf(path, sizeof(path), "/sys/class/rfkill/rfkill%d/type", i);
        FILE *ft = fopen(path, "r");
        if (!ft) break;
        bool match = (fgets(buf, sizeof(buf), ft) != NULL &&
                      strncmp(buf, type, strlen(type)) == 0);
        fclose(ft);
        if (!match) continue;

        snprintf(path, sizeof(path),
                 "/sys/class/rfkill/rfkill%d/soft_blocked", i);
        FILE *fb = fopen(path, "r");
        if (!fb) return "?";
        bool blocked = (fgets(buf, sizeof(buf), fb) != NULL && buf[0] == '1');
        fclose(fb);
        return blocked ? "OFF" : "ON";
    }
    return "?";
}

static const char *status_wifi(void)      { return rfkill_status("wlan");      }
static const char *status_bt  (void)      { return rfkill_status("bluetooth"); }

/* ── Forward declaration (body in pmenu.c, visible via pmenu.h) ───────────── */
/* th_close_menu declared in pmenu.h */

/* ═══════════════════════════════════════════════════════════════════════════
 * Leaf action callbacks
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

/* Delegates to th_close_menu (FSM handler) so the state transition is
 * always driven through the FSM, not duplicated here. */
static void action_exit_menu(void) { th_close_menu(); }

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
Menu g_net_menu = { "Networking", g_net_items, 2, NULL };

static MenuItem g_pwr_items[] = {
    { "Reboot",    action_reboot,   NULL, NULL },
    { "Power Off", action_poweroff, NULL, NULL },
};
Menu g_pwr_menu = { "Power", g_pwr_items, 2, NULL };

static MenuItem g_root_items[] = {
    { "Networking", NULL, &g_net_menu, NULL },
    { "Power",      NULL, &g_pwr_menu, NULL },
    { "Exit Menu",  action_exit_menu, NULL, NULL },
};
const Menu g_root_menu = { "pmOS  GT-I9100", g_root_items, 3, NULL };

/* ═══════════════════════════════════════════════════════════════════════════
 * Rendering — single entry point
 * ═══════════════════════════════════════════════════════════════════════════ */

static void render_menu(void)
{
    /* Breadcrumb: walk from current node to root via parent pointers,
     * collect the path, then print root-first. */
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
        bool        sel    = (i == g_sel);
        bool        has_sub = (g_menu->items[i].submenu != NULL);
        const char *badge  = g_menu->items[i].status
                             ? g_menu->items[i].status() : NULL;
        /*
         * Layout: "| " + label + padding + badge/arrow + " |"
         * badge replaces the submenu arrow when present.
         * badge is right-aligned; label is left-aligned within the remainder.
         */
        const char *arrow  = has_sub ? ">" : " ";
        char        badge_buf[8] = "  ";   /* two spaces when no badge */
        if (badge)
            snprintf(badge_buf, sizeof(badge_buf), "%-3s", badge);

        if (sel)
            printf("| " T_REV T_BOLD "%-*s%s%s" T_RESET T_CYAN " |\r\n",
                   BOX_W - 8, g_menu->items[i].label, badge_buf, arrow);
        else
            printf("| " T_RESET "%-*s%s%s" T_CYAN " |\r\n",
                   BOX_W - 8, g_menu->items[i].label, badge_buf, arrow);
    }

    /* Bottom border */
    fputs(T_CYAN "+", stdout);
    for (int i = 0; i < BOX_W; i++) fputc('-', stdout);
    fputs("+\r\n", stdout);

    fputs(T_DIM "VOL+/-: navigate   PWR: select   BACK: back\r\n"
          T_RESET, stdout);
}

/* Unified render — always clears, then delegates on g_state */
void render(void)
{
    fputs(T_CLEAR, stdout);
    if (g_state == STATE_MENU)
        render_menu();
    fflush(stdout);
}
